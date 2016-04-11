// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include "azure_uamqp_c/wsio.h"
#include "azure_uamqp_c/amqpalloc.h"
#include <windows.h>
#include "winhttp.h"
#include "websocket.h"
#include "azure_c_shared_utility/condition.h"
#include "azure_c_shared_utility/lock.h"
#include "azure_c_shared_utility/xio.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/list.h"
#include "assert.h"

#define DEFAULT_RECEIVE_BUFFER_SIZE 0x1000

typedef enum IO_STATE_TAG
{
    IO_STATE_NOT_OPEN,
    IO_STATE_OPENING,
    IO_STATE_OPEN,
    IO_STATE_CLOSING,
    IO_STATE_ERROR
} IO_STATE;

typedef struct WEBSOCKET_BUFFER_TAG
{
    unsigned char* bytes;
    size_t size;
    ON_SEND_COMPLETE on_send_complete;
    void* callback_context;
    LIST_HANDLE io_list;
} WEBSOCKET_BUFFER;

typedef struct WSIO_INSTANCE_TAG
{
    wchar_t* host;
    int port;
    wchar_t* protocol_name;
    wchar_t* relative_path;

    HINTERNET hOpen;
    HINTERNET hConnect;
    HINTERNET hRequest;
    HINTERNET hWebSocket;

    ON_BYTES_RECEIVED on_bytes_received;
    void* on_bytes_received_context;
    ON_IO_OPEN_COMPLETE on_io_open_complete;
    void* on_io_open_complete_context;
    ON_IO_ERROR on_io_error;
    void* on_io_error_context;
    ON_IO_CLOSE_COMPLETE on_io_close_complete;
    void* on_io_close_complete_context;
    
    LOGGER_LOG logger_log;
    IO_STATE io_state;

    LOCK_HANDLE pending_io_lock;
    LIST_HANDLE pending_io_list;
    int wait_timeout;
    COND_HANDLE received_io;
    LOCK_HANDLE received_io_lock;
    LIST_HANDLE received_io_list;
    WEBSOCKET_BUFFER* receive_buffer;

} WSIO_INSTANCE;


static const IO_INTERFACE_DESCRIPTION ws_io_interface_description =
{
    wsio_create,
    wsio_destroy,
    wsio_open,
    wsio_close,
    wsio_send,
    wsio_dowork,
    wsio_setoption
};


static void begin_send(WSIO_INSTANCE* wsio_instance);
static void begin_receive(WSIO_INSTANCE* wsio_instance);

static void set_io_state(WSIO_INSTANCE* wsio_instance, IO_STATE state)
{
    if (state == IO_STATE_OPENING)
    {
        assert(wsio_instance->io_state == IO_STATE_NOT_OPEN);
        wsio_instance->io_state = IO_STATE_OPENING;
    }

    else if (state == IO_STATE_OPEN)
    {
        assert(wsio_instance->hWebSocket != NULL);
        assert(wsio_instance->io_state == IO_STATE_OPENING || wsio_instance->io_state == IO_STATE_CLOSING);
        wsio_instance->io_state = IO_STATE_OPEN;

        if (wsio_instance->on_io_open_complete != NULL)
        {
            wsio_instance->on_io_open_complete(wsio_instance->on_io_open_complete_context, IO_OPEN_OK);
        }

        begin_send(wsio_instance);
        begin_receive(wsio_instance);
    }

    else if (state == IO_STATE_CLOSING)
    {
        if (wsio_instance->io_state != IO_STATE_OPEN && wsio_instance->io_state != IO_STATE_ERROR)
        {
            // First must be connected before being disconnected
            set_io_state(wsio_instance, IO_STATE_ERROR);
        }

        wsio_instance->io_state = IO_STATE_CLOSING;
    }

    else if (state == IO_STATE_NOT_OPEN)
    {
        assert(wsio_instance->hConnect == NULL);
        if (wsio_instance->io_state != IO_STATE_CLOSING )
        {
            // First must be closing
            set_io_state(wsio_instance, IO_STATE_CLOSING);
        }
        
        wsio_instance->io_state = IO_STATE_NOT_OPEN;
        
        if (wsio_instance->on_io_close_complete)
        {
            wsio_instance->on_io_close_complete(wsio_instance->on_io_close_complete_context);
        }
    }

    else if (state == IO_STATE_ERROR)
    {
        if (wsio_instance->io_state == IO_STATE_OPENING)
        {
            wsio_instance->io_state = IO_STATE_ERROR;

            if (wsio_instance->on_io_open_complete != NULL)
            {
                wsio_instance->on_io_open_complete(wsio_instance->on_io_open_complete_context, IO_OPEN_ERROR);
            }
        }
        else
        {
            wsio_instance->io_state = IO_STATE_ERROR;
            
            if (wsio_instance->on_io_error != NULL)
            {
                wsio_instance->on_io_error(wsio_instance->on_io_error_context);
            }
        }
    }

    else 
    {
        assert(0);
    }
}
static void begin_receive(WSIO_INSTANCE* wsio_instance)
{
    if (wsio_instance->io_state != IO_STATE_OPEN || wsio_instance->receive_buffer != NULL)
    {
        LOG(wsio_instance->logger_log, LOG_LINE, "Failure: Bad state on begin_receive.\r\n");
        set_io_state(wsio_instance, IO_STATE_ERROR);
    }
    else
    {
        wsio_instance->receive_buffer = (WEBSOCKET_BUFFER*)amqpalloc_malloc(sizeof(WEBSOCKET_BUFFER));
        if (wsio_instance->receive_buffer == NULL)
        {
            LOG(wsio_instance->logger_log, LOG_LINE, "Failure: Receive buffer allocation failed.\r\n");
            set_io_state(wsio_instance, IO_STATE_ERROR);
        }
        else
        {
            wsio_instance->receive_buffer->on_send_complete = NULL;
            wsio_instance->receive_buffer->callback_context = NULL;
            wsio_instance->receive_buffer->io_list = NULL;
            wsio_instance->receive_buffer->size = DEFAULT_RECEIVE_BUFFER_SIZE;
            wsio_instance->receive_buffer->bytes = (unsigned char*)amqpalloc_malloc(wsio_instance->receive_buffer->size);
            if (wsio_instance->receive_buffer->bytes == NULL)
            {
                /* Codes_SRS_WSIO_01_055: [If queueing the data fails (i.e. due to insufficient memory), wsio_send shall fail and return a non-zero value.] */
                amqpalloc_free(wsio_instance->receive_buffer);
                wsio_instance->receive_buffer = NULL;

                LOG(wsio_instance->logger_log, LOG_LINE, "Failure: Receive buffer memory allocation failed.\r\n");
                set_io_state(wsio_instance, IO_STATE_ERROR);
            }
            else
            {
                DWORD dwRead;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE tmpType;

                if (0 != WinHttpWebSocketReceive(wsio_instance->hWebSocket, wsio_instance->receive_buffer->bytes, wsio_instance->receive_buffer->size, &dwRead, &tmpType))
                {
                    amqpalloc_free(wsio_instance->receive_buffer->bytes);
                    amqpalloc_free(wsio_instance->receive_buffer);
                    wsio_instance->receive_buffer = NULL;

                    LOG(wsio_instance->logger_log, LOG_LINE, "Failure: on WinHttpWebSocketReceive.\r\n");
                    set_io_state(wsio_instance, IO_STATE_ERROR);
                }
            }
        }
    }
}

static void begin_send(WSIO_INSTANCE* wsio_instance)
{
    LIST_ITEM_HANDLE first_pending_io;
    int result;

    if (wsio_instance->io_state != IO_STATE_OPEN)
    {
        LOG(wsio_instance->logger_log, LOG_LINE, "Failure: Bad state on begin_send.\r\n");
        set_io_state(wsio_instance, IO_STATE_ERROR);
    }
    else
    {
        /* Codes_SRS_WSIO_01_120: [This event shall only be processed if the IO is open.] */
        /* Codes_SRS_WSIO_01_071: [If any pending IO chunks queued in wsio_send are to be sent, then the first one shall be retrieved from the queue.] */
        Lock(wsio_instance->pending_io_lock);
        first_pending_io = list_get_head_item(wsio_instance->pending_io_list);
        Unlock(wsio_instance->pending_io_lock);

        if (first_pending_io != NULL)
        {
            WEBSOCKET_BUFFER* pending_socket_io = (WEBSOCKET_BUFFER*)list_item_get_value(first_pending_io);
            if (pending_socket_io == NULL)
            {
                LOG(wsio_instance->logger_log, LOG_LINE, "Failure: Pending io does not contain buffer.\r\n");
                set_io_state(wsio_instance, IO_STATE_ERROR);
            }
            else
            {
                if (0 != WinHttpWebSocketSend(wsio_instance->hWebSocket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, pending_socket_io->bytes, pending_socket_io->size))
                {
                    Lock(wsio_instance->pending_io_lock);
                    (void)list_remove(wsio_instance->pending_io_list, first_pending_io);
                    Unlock(wsio_instance->pending_io_lock);

                    if (pending_socket_io->on_send_complete != NULL)
                    {
                        pending_socket_io->on_send_complete(pending_socket_io->callback_context, IO_SEND_ERROR);
                    }

                    amqpalloc_free(pending_socket_io->bytes);
                    amqpalloc_free(pending_socket_io);
                }
            }
        }
    }
}

static void end_receive(WSIO_INSTANCE* wsio_instance, DWORD bytes_received)
{
    if (wsio_instance->io_state != IO_STATE_OPEN || wsio_instance->receive_buffer == NULL)
    {
        LOG(wsio_instance->logger_log, LOG_LINE, "Failure: Bad state on end_receive.\r\n");
        set_io_state(wsio_instance, IO_STATE_ERROR);
    }
    else
    {
        assert(wsio_instance->receive_buffer->size >= bytes_received);
        wsio_instance->receive_buffer->size = bytes_received;
        wsio_instance->receive_buffer->io_list = wsio_instance->received_io_list;

        /* Transfer receive buffer to receive buffer list */
        Lock(wsio_instance->received_io_lock);
        if (list_add(wsio_instance->received_io_list, wsio_instance->receive_buffer) == NULL)
        {
            amqpalloc_free(wsio_instance->receive_buffer->bytes);
            amqpalloc_free(wsio_instance->receive_buffer);

            LOG(wsio_instance->logger_log, LOG_LINE, "Failure: Adding adding received buffer.\r\n");
            set_io_state(wsio_instance, IO_STATE_ERROR);
        }
        else
        {
            Condition_Post(wsio_instance->received_io);
        }
        Unlock(wsio_instance->received_io_lock);

        /* Ready for next */
        wsio_instance->receive_buffer = NULL;
    }
}

static void end_send(WSIO_INSTANCE* wsio_instance, DWORD bytes_sent)
{
    LIST_ITEM_HANDLE first_pending_io;
    WEBSOCKET_BUFFER* pending_socket_io = NULL;

    Lock(wsio_instance->pending_io_lock);

    first_pending_io = list_get_head_item(wsio_instance->pending_io_list);
    if (first_pending_io != NULL)
    {
        pending_socket_io = (WEBSOCKET_BUFFER*)list_item_get_value(first_pending_io);
        (void)list_remove(wsio_instance->pending_io_list, first_pending_io);
    }

    Unlock(wsio_instance->pending_io_lock);

    if (pending_socket_io == NULL)
    {
        LOG(wsio_instance->logger_log, LOG_LINE, "Failure: No pending socket io anymore.\r\n");
        set_io_state(wsio_instance, IO_STATE_ERROR);
    }
    else
    {
        assert(bytes_sent == pending_socket_io->size);

        if (pending_socket_io->on_send_complete != NULL)
        {
            pending_socket_io->on_send_complete(pending_socket_io->callback_context, IO_SEND_OK);
        }

        amqpalloc_free(pending_socket_io->bytes);
        amqpalloc_free(pending_socket_io);

        /* Send more */
        begin_send(wsio_instance);
    }
}

static int queue_buffer(LIST_HANDLE io_list, const unsigned char* buffer, size_t size, ON_SEND_COMPLETE on_send_complete, void* callback_context)
{
    int result = 0;

    WEBSOCKET_BUFFER* websocket_buffer = (WEBSOCKET_BUFFER*)amqpalloc_malloc(sizeof(WEBSOCKET_BUFFER));
    if (buffer == NULL)
    {
        /* Codes_SRS_WSIO_01_055: [If queueing the data fails (i.e. due to insufficient memory), wsio_send shall fail and return a non-zero value.] */
        result = __LINE__;
    }
    else
    {
        websocket_buffer->bytes = (unsigned char*)amqpalloc_malloc(size);
        if (websocket_buffer->bytes == NULL)
        {
            /* Codes_SRS_WSIO_01_055: [If queueing the data fails (i.e. due to insufficient memory), wsio_send shall fail and return a non-zero value.] */
            amqpalloc_free(websocket_buffer);
            result = __LINE__;
        }
        else
        {
            websocket_buffer->size = size;
            websocket_buffer->on_send_complete = on_send_complete;
            websocket_buffer->callback_context = callback_context;
            websocket_buffer->io_list = io_list;

            (void)memcpy(websocket_buffer->bytes, buffer, size);

            /* Codes_SRS_WSIO_01_105: [The data and callback shall be queued by calling list_add on the list created in wsio_create.] */
            if (list_add(io_list, websocket_buffer) == NULL)
            {
                /* Codes_SRS_WSIO_01_055: [If queueing the data fails (i.e. due to insufficient memory), wsio_send shall fail and return a non-zero value.] */
                amqpalloc_free(websocket_buffer->bytes);
                amqpalloc_free(websocket_buffer);
                result = __LINE__;
            }
            else
            {
                result = 0;
            }
        }
    }

    return result;
}

inline void log_winhttp_status(LOGGER_LOG logger, DWORD status, LPVOID lpvStatusInformation, DWORD dwStatusInformationLength) 
{
    switch (status) 
    {
    case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER:
        LOG(logger, LOG_LINE, "Connected to %S.\r\n", (wchar_t*)lpvStatusInformation); break;
    case WINHTTP_CALLBACK_STATUS_RESOLVING_NAME: 
        LOG(logger, LOG_LINE, "Resolving %S...\r\n", (wchar_t*)lpvStatusInformation); break;
    case WINHTTP_CALLBACK_STATUS_NAME_RESOLVED: 
        LOG(logger, LOG_LINE, "Resolved (%S).\r\n", lpvStatusInformation ? (wchar_t*)lpvStatusInformation : L"???"); break;
    case WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER:
        LOG(logger, LOG_LINE, "Connecting to %S...\r\n", (wchar_t*)lpvStatusInformation); break;
    case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST:
        LOG(logger, LOG_LINE, "Sending Request...\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_SENT:
        LOG(logger, LOG_LINE, "   Sent %d bytes ...\r\n", (int)(*(DWORD*)lpvStatusInformation)); break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        LOG(logger, LOG_LINE, "Request sent.\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_RECEIVING_RESPONSE:
        LOG(logger, LOG_LINE, "Receiving Response...\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED:
        LOG(logger, LOG_LINE, "   Received %d bytes ...\r\n", (int)(*(DWORD*)lpvStatusInformation)); break;
    case WINHTTP_CALLBACK_STATUS_CLOSING_CONNECTION:
        LOG(logger, LOG_LINE, "Closing connection...\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_CONNECTION_CLOSED:
        LOG(logger, LOG_LINE, "Connection closed.\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_DETECTING_PROXY:
        LOG(logger, LOG_LINE, "Detecting proxy...\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_REDIRECT:
        LOG(logger, LOG_LINE, "Redirecting to %S...\r\n", (wchar_t*)lpvStatusInformation); break;
    case WINHTTP_CALLBACK_STATUS_INTERMEDIATE_RESPONSE:
        LOG(logger, LOG_LINE, "Intermediate response %d...\r\n", (int)(*(DWORD*)lpvStatusInformation)); break;
    case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
        LOG(logger, LOG_LINE, "Secure socket failure!\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        LOG(logger, LOG_LINE, "Request error!\r\n"); break;
#ifdef _DEBUG
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        LOG(logger, LOG_LINE, "WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        LOG(logger, LOG_LINE, "WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        LOG(logger, LOG_LINE, "WINHTTP_CALLBACK_STATUS_READ_COMPLETE\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
        LOG(logger, LOG_LINE, "WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE:
        LOG(logger, LOG_LINE, "WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE\r\n"); break;
    case WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE:
        LOG(logger, LOG_LINE, "WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE\r\n"); break;
#endif
    }
}

static void CALLBACK wsio_on_status_callback(HINTERNET hInternet, DWORD_PTR dwContext, DWORD dwInternetStatus, LPVOID lpvStatusInformation, DWORD dwStatusInformationLength)
{
    WSIO_INSTANCE* wsio_instance = (WSIO_INSTANCE*)dwContext;

    if (wsio_instance != NULL)
    {
#ifdef _DEBUG
        log_winhttp_status(wsio_instance->logger_log, dwInternetStatus, lpvStatusInformation, dwStatusInformationLength);
#endif 
        if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE)
        {
            assert(hInternet == wsio_instance->hRequest);

            if (!WinHttpReceiveResponse(hInternet, 0))
            {
                LOG(wsio_instance->logger_log, LOG_LINE, "Error WinHttpReceiveResponse %d.\r\n", GetLastError());
                set_io_state(wsio_instance, IO_STATE_ERROR);
            }
        }

        else if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)
        {
            DWORD status_code;
            DWORD size = sizeof(status_code);
            if (hInternet != wsio_instance->hRequest)
            {
                LOG(wsio_instance->logger_log, LOG_LINE, "Bad handle passed, %x.\r\n", (int)hInternet);
                set_io_state(wsio_instance, IO_STATE_ERROR);
            }
            else if (!WinHttpQueryHeaders(hInternet, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status_code, &size, NULL))
            {
                LOG(wsio_instance->logger_log, LOG_LINE, "Error WinHttpQueryHeaders %d.\r\n", GetLastError());
                set_io_state(wsio_instance, IO_STATE_ERROR);
            }
            else if (status_code != HTTP_STATUS_SWITCH_PROTOCOLS)
            {
                LOG(wsio_instance->logger_log, LOG_LINE, "Error switching protocols.\r\n");
                set_io_state(wsio_instance, IO_STATE_ERROR);
            }
            else
            {
                assert(wsio_instance->hWebSocket == NULL);

                /* Complete the upgrade to web socket */
                wsio_instance->hWebSocket = WinHttpWebSocketCompleteUpgrade(hInternet, 0);
                if (wsio_instance->hWebSocket == NULL)
                {
                    LOG(wsio_instance->logger_log, LOG_LINE, "Failed upgrading %d.\r\n", GetLastError());
                    set_io_state(wsio_instance, IO_STATE_ERROR);
                }
                else if (!WinHttpSetOption(wsio_instance->hWebSocket, WINHTTP_OPTION_CONTEXT_VALUE, &wsio_instance, sizeof(wsio_instance)))
                {
                    LOG(wsio_instance->logger_log, LOG_LINE, "Failed attaching context %d.\r\n", GetLastError());
                    (void)WinHttpWebSocketClose(wsio_instance->hWebSocket, WINHTTP_WEB_SOCKET_ABORTED_CLOSE_STATUS, NULL, 0);
                    wsio_instance->hWebSocket = NULL;
                    set_io_state(wsio_instance, IO_STATE_ERROR);
                }
                else
                {
                    /* Close request handle now that we have a socket*/
                    (void)WinHttpCloseHandle(hInternet);

                    /* Now we are open for send / receive business, send any pending io and begin receiving */
                    set_io_state(wsio_instance, IO_STATE_OPEN);
                }
            }
        }

        else if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
        {
            if (wsio_instance->hWebSocket != NULL)
            {
                WINHTTP_WEB_SOCKET_STATUS* webSockStatus = (WINHTTP_WEB_SOCKET_STATUS *)lpvStatusInformation;
                if (wsio_instance->on_bytes_received)
                {
                    switch (webSockStatus->eBufferType)
                    {
                    case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:
                    case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE:
                        /* Buffer contains either the entire message or the last part of it. */
                        end_receive(wsio_instance, webSockStatus->dwBytesTransferred);
                        break;
                    case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:
                    case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
                        /* Buffer contains only part of a message. */
                        end_receive(wsio_instance, webSockStatus->dwBytesTransferred);
                        break;
                    case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE:
                        /* What now? */
                        break;
                    }
                }

                /* Continue receiving */
                begin_receive(wsio_instance);
            }
        }

        else if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE)
        {
            if (wsio_instance->hWebSocket != NULL)
            {
                end_send(wsio_instance, ((WINHTTP_WEB_SOCKET_STATUS *)lpvStatusInformation)->dwBytesTransferred);
            }
        }

        else if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE)
        {
            /* The connection was successfully closed via a call to WinHttpWebSocketClose */
            (void)WinHttpCloseHandle(wsio_instance->hWebSocket);
            if (wsio_instance->hOpen == NULL)
            {
                (void)WinHttpCloseHandle(wsio_instance->hConnect);
                wsio_instance->hConnect = NULL;
            }
        }

        else if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
        {
            if (wsio_instance->hWebSocket == hInternet)
            {
                /* After upgrade succeeded, request should be NULL */
                assert(wsio_instance->hRequest == NULL);

                wsio_instance->hWebSocket = NULL;

                /* Transition from open to closing */
                set_io_state(wsio_instance, IO_STATE_CLOSING);

                if (wsio_instance->hConnect)
                {
                    (void)WinHttpCloseHandle(wsio_instance->hConnect);
                }
            }

            else if (wsio_instance->hRequest == hInternet)
            {
                wsio_instance->hRequest = NULL;

                if (wsio_instance->hWebSocket == NULL)
                {
                    /* Closed pre-upgrade, transition to closing */
                    set_io_state(wsio_instance, IO_STATE_CLOSING);
                }
                else
                {
                    /* The upgrade went through and the request is released */
                }
            }

            else if (wsio_instance->hConnect == hInternet)
            {
                wsio_instance->hConnect = NULL;

                /* Connection fully closed, transition back to not open */
                set_io_state(wsio_instance, IO_STATE_NOT_OPEN);
            }

            else
            {
                /* This should not happen as for hOpen we unregistered the context */
                assert(0);
            }
        }

        else if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE)
        {
            LOG(wsio_instance->logger_log, LOG_LINE, "Failure: winhttp returned WINHTTP_CALLBACK_STATUS_SECURE_FAILURE.\r\n");
            set_io_state(wsio_instance, IO_STATE_ERROR);
        }

        else if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
        {
            LOG(wsio_instance->logger_log, LOG_LINE, "Error during request %d\r\n", ((WINHTTP_ASYNC_RESULT*)lpvStatusInformation)->dwError);
            set_io_state(wsio_instance, IO_STATE_ERROR);
        }
    }
}

CONCRETE_IO_HANDLE wsio_create(void* io_create_parameters, LOGGER_LOG logger_log)
{
    /* Codes_SRS_WSIO_01_003: [io_create_parameters shall be used as a WSIO_CONFIG*.] */
    WSIO_CONFIG* ws_io_config = (WSIO_CONFIG*)io_create_parameters;
    WSIO_INSTANCE* result;

    if ((ws_io_config == NULL) ||
        (ws_io_config->host == NULL) ||
        (ws_io_config->protocol_name == NULL) ||
        (ws_io_config->relative_path == NULL))
    {
        result = NULL;
    }
    else
    {
        /* Codes_SRS_WSIO_01_001: [wsio_create shall create an instance of a wsio and return a non-NULL handle to it.] */
        result = (WSIO_INSTANCE*)amqpalloc_malloc(sizeof(WSIO_INSTANCE));
        if (result != NULL)
        {
            memset(result, 0, sizeof(WSIO_INSTANCE));
            result->logger_log = logger_log;

            /* Codes_SRS_WSIO_01_098: [wsio_create shall create a pending IO list that is to be used when sending buffers over the libwebsockets IO by calling list_create.] */
            result->pending_io_list = list_create();
            if (result->pending_io_list == NULL)
            {
                /* Codes_SRS_WSIO_01_099: [If list_create fails then wsio_create shall fail and return NULL.] */
                wsio_destroy(result);
                result = NULL;
            }
            else
            {
                result->received_io_list = list_create();
                if (result->received_io_list == NULL)
                {
                    /* Codes_SRS_WSIO_01_099: [If list_create fails then wsio_create shall fail and return NULL.] */
                    wsio_destroy(result);
                    result = NULL;
                }
                else
                {
                    result->pending_io_lock = Lock_Init();
                    result->received_io_lock = Lock_Init();
                    result->received_io = Condition_Init();

                    if ((result->received_io_lock == NULL) || 
                        (result->received_io == NULL) || 
                        (result->pending_io_lock == NULL))
                    {
                        wsio_destroy(result);
                        result = NULL;
                    }
                    else
                    {
                        int string_size = MultiByteToWideChar(CP_ACP, 0, ws_io_config->host, -1, NULL, 0);
                        if (string_size <= 0)
                        {
                            wsio_destroy(result);
                            result = NULL;
                        }
                        else
                        {
                            result->host = (wchar_t*)amqpalloc_malloc((string_size + 1) * sizeof(wchar_t));
                            if ((result->host == NULL) || MultiByteToWideChar(CP_ACP, 0, ws_io_config->host, -1, result->host, string_size) == 0)
                            {
                                wsio_destroy(result);
                                result = NULL;
                            }
                            else
                            {
                                string_size = MultiByteToWideChar(CP_ACP, 0, ws_io_config->protocol_name, -1, NULL, 0);
                                if (string_size <= 0)
                                {
                                    wsio_destroy(result);
                                    result = NULL;
                                }
                                else
                                {
                                    result->protocol_name = (wchar_t*)amqpalloc_malloc((string_size + 1) * sizeof(wchar_t));
                                    if ((result->protocol_name == NULL) || MultiByteToWideChar(CP_ACP, 0, ws_io_config->protocol_name, -1, result->protocol_name, string_size) == 0)
                                    {
                                        wsio_destroy(result);
                                        result = NULL;
                                    }
                                    else
                                    {
                                        string_size = MultiByteToWideChar(CP_ACP, 0, ws_io_config->relative_path, -1, NULL, 0);
                                        if (string_size <= 0)
                                        {
                                            wsio_destroy(result);
                                            result = NULL;
                                        }
                                        else
                                        {
                                            result->relative_path = (wchar_t*)amqpalloc_malloc((string_size + 1) * sizeof(wchar_t));
                                            if ((result->relative_path == NULL) || MultiByteToWideChar(CP_ACP, 0, ws_io_config->relative_path, -1, result->relative_path, string_size) == 0)
                                            {
                                                wsio_destroy(result);
                                                result = NULL;
                                            }
                                            else
                                            {
                                                result->hOpen = WinHttpOpen(NULL, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, WINHTTP_FLAG_ASYNC);
                                                if (result->hOpen == NULL || !WinHttpSetOption(result->hOpen, WINHTTP_OPTION_CONTEXT_VALUE, &result, sizeof(result)))
                                                {
                                                    LOG(result->logger_log, LOG_LINE, "Error WinHttpOpen %d.\r\n", GetLastError());
                                                    wsio_destroy(result);
                                                    result = NULL;
                                                }
                                                else
                                                {
                                                    if (WINHTTP_INVALID_STATUS_CALLBACK == WinHttpSetStatusCallback(result->hOpen, wsio_on_status_callback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0))
                                                    {
                                                        LOG(result->logger_log, LOG_LINE, "Error WinHttpSetStatusCallback %d.\r\n", GetLastError());
                                                        wsio_destroy(result);
                                                        result = NULL;
                                                    }
                                                    else
                                                    {
                                                        /* Success, declare us created but not open */
                                                        result->port = ws_io_config->port;
                                                        result->io_state = IO_STATE_NOT_OPEN;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return result;
}

void wsio_destroy(CONCRETE_IO_HANDLE ws_io)
{
    /* Codes_SRS_WSIO_01_008: [If ws_io is NULL, wsio_destroy shall do nothing.] */
    if (ws_io != NULL)
    {
        WSIO_INSTANCE* wsio_instance = (WSIO_INSTANCE*)ws_io;

        /* Codes_SRS_WSIO_01_009: [wsio_destroy shall execute a close action if the IO has already been open or an open action is already pending.] */
        (void)wsio_close(wsio_instance, NULL, NULL);
        
        /* Codes_SRS_WSIO_01_007: [wsio_destroy shall free all resources associated with the wsio instance.] */
        if (wsio_instance->hOpen != NULL)
        {
            DWORD_PTR null = 0;
            (void)WinHttpSetOption(wsio_instance->hConnect, WINHTTP_OPTION_CONTEXT_VALUE, &null, sizeof(null));

            (void)WinHttpCloseHandle(wsio_instance->hOpen);
            wsio_instance->hOpen = NULL;
        }

        if (wsio_instance->host != NULL)
        {
            amqpalloc_free(wsio_instance->host);
            wsio_instance->host = NULL;
        }

        if (wsio_instance->relative_path != NULL)
        {
            amqpalloc_free(wsio_instance->relative_path);
            wsio_instance->relative_path = NULL;
        }

        if (wsio_instance->protocol_name != NULL)
        {
            amqpalloc_free(wsio_instance->protocol_name);
            wsio_instance->protocol_name = NULL;
        }

        if (wsio_instance->received_io_list != NULL)
        {
            list_destroy(wsio_instance->received_io_list);
            wsio_instance->received_io_list = NULL;
        }

        if (wsio_instance->pending_io_list != NULL)
        {
            list_destroy(wsio_instance->pending_io_list);
            wsio_instance->pending_io_list = NULL;
        }

        if (wsio_instance->received_io_lock != NULL)
        {
            Lock_Deinit(wsio_instance->received_io_lock);
            wsio_instance->received_io_lock = NULL;
        }

        if (wsio_instance->pending_io_lock != NULL)
        {
            Lock_Deinit(wsio_instance->pending_io_lock);
            wsio_instance->pending_io_lock = NULL;
        }

        if (wsio_instance->received_io != NULL)
        {
            Condition_Deinit(wsio_instance->received_io);
            wsio_instance->received_io = NULL;
        }

        amqpalloc_free(ws_io);
    }
}

#define PROTOCOL_HEADER L"Sec-WebSocket-Protocol: "

int wsio_open(CONCRETE_IO_HANDLE ws_io, ON_IO_OPEN_COMPLETE on_io_open_complete, void* on_io_open_complete_context, ON_BYTES_RECEIVED on_bytes_received, void* on_bytes_received_context, ON_IO_ERROR on_io_error, void* on_io_error_context)
{
    int result = 0;

    if (ws_io == NULL)
    {
        result = __LINE__;
    }
    else
    {
        WSIO_INSTANCE* wsio_instance = (WSIO_INSTANCE*)ws_io;

        /* Codes_SRS_WSIO_01_034: [If another open is in progress or has completed successfully (the IO is open), wsio_open shall fail and return a non-zero value without performing any connection related activities.] */
        if (wsio_instance->io_state != IO_STATE_NOT_OPEN)
        {
            result = __LINE__;
        }
        else
        {
            wsio_instance->on_bytes_received = on_bytes_received;
            wsio_instance->on_bytes_received_context = on_bytes_received_context;
            wsio_instance->on_io_open_complete = on_io_open_complete;
            wsio_instance->on_io_open_complete_context = on_io_open_complete_context;
            wsio_instance->on_io_error = on_io_error;
            wsio_instance->on_io_error_context = on_io_error_context;
            wsio_instance->io_state = IO_STATE_NOT_OPEN;

            wsio_instance->hConnect = WinHttpConnect(wsio_instance->hOpen, wsio_instance->host, wsio_instance->port, 0);
            if (wsio_instance->hConnect == NULL)
            {
                result = __LINE__;
            }
            else
            {
                if (0 == WinHttpSetOption(wsio_instance->hConnect, WINHTTP_OPTION_CONTEXT_VALUE, &wsio_instance, sizeof(wsio_instance)))
                {
                    (void)WinHttpCloseHandle(wsio_instance->hConnect);
                    wsio_instance->hConnect = NULL;
                    result = __LINE__;
                }
                else
                {
                    wsio_instance->hRequest = WinHttpOpenRequest(wsio_instance->hConnect, L"GET", wsio_instance->relative_path, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
                    if (wsio_instance->hRequest == NULL)
                    {
                        (void)WinHttpCloseHandle(wsio_instance->hConnect);
                        wsio_instance->hConnect = NULL;
                        result = __LINE__;
                    }
                    else
                    {
                        rsize_t length = wcslen(PROTOCOL_HEADER) + wcslen(wsio_instance->protocol_name) + 3; // For CR/LF and \0
                        wchar_t* protocol_header = (wchar_t*)amqpalloc_malloc(length * sizeof(wchar_t));
                        if (protocol_header == NULL)
                        {
                            result = __LINE__;
                        }
                        else
                        {
                            if (0 != wcscpy_s(protocol_header, length, PROTOCOL_HEADER) ||
                                0 != wcscat_s(protocol_header, length, wsio_instance->protocol_name) ||
                                0 != wcscat_s(protocol_header, length, L"\r\n"))
                            {
                                result = __LINE__;
                            }
                            else
                            {
                                protocol_header[length - 1] = L'\0';

                                if (!WinHttpSetOption(wsio_instance->hRequest, WINHTTP_OPTION_CONTEXT_VALUE, &wsio_instance, sizeof(wsio_instance)) ||
                                    !WinHttpSetOption(wsio_instance->hRequest, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) ||
                                    !WinHttpAddRequestHeaders(wsio_instance->hRequest, protocol_header, -1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
                                {
                                    (void)WinHttpCloseHandle(wsio_instance->hRequest);
                                    (void)WinHttpCloseHandle(wsio_instance->hConnect);
                                    wsio_instance->hRequest = NULL;
                                    wsio_instance->hConnect = NULL;
                                    result = __LINE__;
                                }
                                else
                                {
                                    wsio_instance->io_state = IO_STATE_OPENING;

                                    if (0 == WinHttpSendRequest(wsio_instance->hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0))
                                    {
                                        (void)WinHttpCloseHandle(wsio_instance->hRequest);
                                        (void)WinHttpCloseHandle(wsio_instance->hConnect);
                                        wsio_instance->hRequest = NULL;
                                        wsio_instance->hConnect = NULL;

                                        wsio_instance->io_state = IO_STATE_NOT_OPEN;
                                        result = __LINE__;
                                    }
                                    else
                                    {
                                        result = 0;
                                    }
                                }
                            }
                            amqpalloc_free(protocol_header);
                        }
                    }
                }
            }
        }
    }

    return result;
}

int wsio_close(CONCRETE_IO_HANDLE ws_io, ON_IO_CLOSE_COMPLETE on_io_close_complete, void* on_io_close_complete_context)
{
    int result = 0;

    if (ws_io == NULL)
    {
        /* Codes_SRS_WSIO_01_042: [if ws_io is NULL, wsio_close shall return a non-zero value.] */
        result = __LINE__;
    }
    else
    {
        WSIO_INSTANCE* wsio_instance = (WSIO_INSTANCE*)ws_io;

        /* Codes_SRS_WSIO_01_045: [wsio_close when no open action has been issued shall fail and return a non-zero value.] */
        /* Codes_SRS_WSIO_01_046: [wsio_close after a wsio_close shall fail and return a non-zero value.] */
        if ((wsio_instance->io_state == IO_STATE_NOT_OPEN) ||
            (wsio_instance->io_state == IO_STATE_CLOSING))
        {
            result = __LINE__;
        }
        else
        {
            /* cancel all pending IOs */
            LIST_ITEM_HANDLE first_pending_io;

            /* Codes_SRS_WSIO_01_108: [wsio_close shall obtain all the pending IO items by repetitively querying for the head of the pending IO list and freeing that head item.] */
            /* Codes_SRS_WSIO_01_111: [Obtaining the head of the pending IO list shall be done by calling list_get_head_item.] */
            Lock(wsio_instance->pending_io_lock);
            while ((first_pending_io = list_get_head_item(wsio_instance->pending_io_list)) != NULL)
            {
                WEBSOCKET_BUFFER* pending_socket_io = (WEBSOCKET_BUFFER*)list_item_get_value(first_pending_io);

                if (pending_socket_io != NULL)
                {
                    /* Codes_SRS_WSIO_01_060: [The argument on_send_complete shall be optional, if NULL is passed by the caller then no send complete callback shall be triggered.] */
                    if (pending_socket_io->on_send_complete != NULL)
                    {
                        /* Codes_SRS_WSIO_01_109: [For each pending item the send complete callback shall be called with IO_SEND_CANCELLED.] */
                        /* Codes_SRS_WSIO_01_110: [The callback context passed to the on_send_complete callback shall be the context given to wsio_send.] */
                        /* Codes_SRS_WSIO_01_059: [The callback_context argument shall be passed to on_send_complete as is.] */
                        pending_socket_io->on_send_complete(pending_socket_io->callback_context, IO_SEND_CANCELLED);
                    }

                    if (pending_socket_io != NULL)
                    {
                        amqpalloc_free(pending_socket_io->bytes);
                        amqpalloc_free(pending_socket_io);
                    }
                }

                (void)list_remove(wsio_instance->pending_io_list, first_pending_io);
            }
            Unlock(wsio_instance->pending_io_lock);

            wsio_instance->on_io_close_complete = on_io_close_complete;
            wsio_instance->on_io_close_complete_context = on_io_close_complete_context;

            /* Got into closing state and kick off closing by closing a handle */
            set_io_state(wsio_instance, IO_STATE_CLOSING);

            if (wsio_instance->hWebSocket != NULL)
            {
                /* We are connected, close the web socket, which will clean up the other handles */
                (void)WinHttpWebSocketClose(wsio_instance->hWebSocket, WINHTTP_WEB_SOCKET_ENDPOINT_TERMINATED_CLOSE_STATUS, NULL, 0);
            }
            else
            {
                /* We are not connected but in the process of connecting, close the request and connection */
                if (wsio_instance->hRequest)
                {
                    (void)WinHttpCloseHandle(wsio_instance->hRequest);
                }

                if (wsio_instance->hConnect)
                {
                    (void)WinHttpCloseHandle(wsio_instance->hConnect);
                }
            }

            /* Codes_SRS_WSIO_01_044: [On success wsio_close shall return 0.] */
            result = 0;
        }
    }
    return result;
}

/* Codes_SRS_WSIO_01_050: [wsio_send shall send the buffer bytes through the websockets connection.] */
int wsio_send(CONCRETE_IO_HANDLE ws_io, const void* buffer, size_t size, ON_SEND_COMPLETE on_send_complete, void* callback_context)
{
    int result;

    /* Codes_SRS_WSIO_01_052: [If any of the arguments ws_io or buffer are NULL, wsio_send shall fail and return a non-zero value.] */
    if ((ws_io == NULL) ||
        (buffer == NULL) ||
        /* Codes_SRS_WSIO_01_053: [If size is zero then wsio_send shall fail and return a non-zero value.] */
        (size == 0))
    {
        result = __LINE__;
    }
    else
    {
        WSIO_INSTANCE* wsio_instance = (WSIO_INSTANCE*)ws_io;

        /* Codes_SRS_WSIO_01_051: [If the wsio is not OPEN (open has not been called or is still in progress) then wsio_send shall fail and return a non-zero value.] */
        if (wsio_instance->io_state != IO_STATE_OPEN)
        {
            result = __LINE__;
        }
        else
        {
            bool send_queue_empty = false;

            Lock(wsio_instance->pending_io_lock);
            send_queue_empty = (NULL == list_get_head_item(wsio_instance->pending_io_list));
            result = queue_buffer(wsio_instance->pending_io_list, (const unsigned char*)buffer, size, on_send_complete, callback_context);
            Unlock(wsio_instance->pending_io_lock);

            if (result != 0)
            {
                result = __LINE__;
            }
            else if (send_queue_empty)
            {
                /* if list was empty, then no send can be in progress yet and we have to kick it off */
                begin_send(wsio_instance);

                if (wsio_instance->io_state != IO_STATE_OPEN)
                {
                    result = __LINE__;
                }
                else
                {
                    result = 0;
                }
            }
        }
    }

    return result;
}

void wsio_dowork(CONCRETE_IO_HANDLE ws_io)
{
    /* Codes_SRS_WSIO_01_063: [If the ws_io argument is NULL, wsio_dowork shall do nothing.] */
    if (ws_io != NULL)
    {
        WSIO_INSTANCE* wsio_instance = (WSIO_INSTANCE*)ws_io;

        /* Codes_SRS_WSIO_01_062: [This shall be done if the IO is not closed.] */
        if ((wsio_instance->io_state == IO_STATE_OPEN) ||
            (wsio_instance->io_state == IO_STATE_OPENING))
        {
            /* deliver all received buffers on the worker thread rather than from callback thread which runs async */
            LIST_ITEM_HANDLE first_received_io;

            Lock(wsio_instance->received_io_lock);
            first_received_io = list_get_head_item(wsio_instance->received_io_list);
            if (first_received_io == NULL && wsio_instance->wait_timeout > 0)
            {
                /* Wait until io received or timeout */
                (void)Condition_Wait(wsio_instance->received_io, wsio_instance->received_io_lock, wsio_instance->wait_timeout);
                first_received_io = list_get_head_item(wsio_instance->received_io_list);
            }

            /* Check again*/
            while (first_received_io != NULL)
            {
                WEBSOCKET_BUFFER* received_socket_io = (WEBSOCKET_BUFFER*)list_item_get_value(first_received_io);
                (void)list_remove(wsio_instance->received_io_list, first_received_io);

                /* Unlock so we do not call under lock*/
                Unlock(wsio_instance->received_io_lock);

                if (received_socket_io != NULL)
                {
                    if (wsio_instance->on_bytes_received != NULL)
                    {
                        wsio_instance->on_bytes_received(wsio_instance->on_bytes_received_context, received_socket_io->bytes, received_socket_io->size);
                    }

                    amqpalloc_free(received_socket_io->bytes);
                    amqpalloc_free(received_socket_io);
                }

                Lock(wsio_instance->received_io_lock);
                first_received_io = list_get_head_item(wsio_instance->received_io_list);
            }
            Unlock(wsio_instance->received_io_lock);
        }
    }
}

int wsio_setoption(CONCRETE_IO_HANDLE ws_io, const char* optionName, const void* value)
{
    int result;
    if (ws_io == NULL)
    {
        result = __LINE__;
    }
    else
    {
        WSIO_INSTANCE* wsio_instance = (WSIO_INSTANCE*)ws_io;
        if (optionName[0] == 't')
        {
            if (value == NULL)
            {
                result = __LINE__;
            }
            else
            {
                wsio_instance->wait_timeout = *(int*)value;
                result = 0;
            }
        }
        else if (optionName[0] == 'c')
        {
            /* Cancel the work thread if it is waiting */
            wsio_instance->wait_timeout = 0;
            Condition_Post(wsio_instance->received_io);
            result = 0;
        }
        else
        {
            result = __LINE__;
        }
    }
    return result;
}

/* Codes_SRS_WSIO_01_064: [wsio_get_interface_description shall return a pointer to an IO_INTERFACE_DESCRIPTION structure that contains pointers to the functions: wsio_create, wsio_destroy, wsio_open, wsio_close, wsio_send and wsio_dowork.] */
const IO_INTERFACE_DESCRIPTION* wsio_get_interface_description(void)
{
    return &ws_io_interface_description;
}

