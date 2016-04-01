// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "testrunnerswitcher.h"
#include "micromock.h"
#include "micromockcharstararenullterminatedstrings.h"
#include "wsio.h"
#include "azure_c_shared_utility/xio.h"
#include "azure_c_shared_utility/list.h"
#include "azure_c_shared_utility/lock.h"
#include "azure_c_shared_utility/condition.h"
#define _WINHTTP_INTERNAL_
#undef WINAPI
#define WINAPI
#include "winhttp.h"

extern "C"
{
#include "../../src/wsio_win32.c"
}

static const void** list_items = NULL;
static size_t list_item_count = 0;

static const LIST_HANDLE TEST_LIST_HANDLE = (LIST_HANDLE)0x4242;
static const LIST_ITEM_HANDLE TEST_LIST_ITEM_HANDLE = (LIST_ITEM_HANDLE)0x11;
static void* TEST_USER_CONTEXT = (void*)0x4244;
static const LOCK_HANDLE TEST_LOCK_HANDLE = (LOCK_HANDLE)0x4244;
static const COND_HANDLE TEST_COND_HANDLE = (COND_HANDLE)0x4247;
static const HINTERNET TEST_HOPEN = (HINTERNET)0x2333;
static const HINTERNET TEST_HREQUEST = (HINTERNET)0x6666;
static const HINTERNET TEST_HCONNECT = (HINTERNET)0x2342;
static const HINTERNET TEST_HWEBSOCKET = (HINTERNET)0x1324;

static WSIO_CONFIG default_wsio_win32_config = 
{
    "test_host",
    443,
    "test_ws_protocol",
    "a/b/c",
    false,
    "my_trusted_ca_payload"
};

static LIST_ITEM_HANDLE add_to_list(const void* item)
{
    const void** items = (const void**)realloc(list_items, (list_item_count + 1) * sizeof(item));
    if (items != NULL)
    {
        list_items = items;
        list_items[list_item_count++] = item;
    }
    return (LIST_ITEM_HANDLE)list_item_count;
}

TYPED_MOCK_CLASS(wsio_win32_mocks, CGlobalMock)
{
public:
	/* amqpalloc mocks */
	MOCK_STATIC_METHOD_1(, void*, amqpalloc_malloc, size_t, size)
	MOCK_METHOD_END(void*, malloc(size));
	MOCK_STATIC_METHOD_2(, void*, amqpalloc_realloc, void*, ptr, size_t, size)
	MOCK_METHOD_END(void*, realloc(ptr, size));
	MOCK_STATIC_METHOD_1(, void, amqpalloc_free, void*, ptr)
		free(ptr);
	MOCK_VOID_METHOD_END();

    // list mocks
    MOCK_STATIC_METHOD_0(, LIST_HANDLE, list_create)
    MOCK_METHOD_END(LIST_HANDLE, TEST_LIST_HANDLE);
    MOCK_STATIC_METHOD_1(, void, list_destroy, LIST_HANDLE, list)
    MOCK_VOID_METHOD_END();
    MOCK_STATIC_METHOD_2(, int, list_remove, LIST_HANDLE, list, LIST_ITEM_HANDLE, item)
        size_t index = (size_t)item - 1;
        (void)memmove(&list_items[index], &list_items[index + 1], sizeof(const void*) * (list_item_count - index - 1));
        list_item_count--;
    MOCK_METHOD_END(int, 0);

    MOCK_STATIC_METHOD_1(, LIST_ITEM_HANDLE, list_get_head_item, LIST_HANDLE, list)
        LIST_ITEM_HANDLE list_item_handle = NULL;
        if (list_item_count > 0)
        {
            list_item_handle = (LIST_ITEM_HANDLE)1;
        }
        else
        {
            list_item_handle = NULL;
        }
    MOCK_METHOD_END(LIST_ITEM_HANDLE, list_item_handle);

    MOCK_STATIC_METHOD_2(, LIST_ITEM_HANDLE, list_add, LIST_HANDLE, list, const void*, item)
    MOCK_METHOD_END(LIST_ITEM_HANDLE, add_to_list(item));
    MOCK_STATIC_METHOD_1(, const void*, list_item_get_value, LIST_ITEM_HANDLE, item_handle)
    MOCK_METHOD_END(const void*, (const void*)list_items[(size_t)item_handle - 1]);
    MOCK_STATIC_METHOD_3(, LIST_ITEM_HANDLE, list_find, LIST_HANDLE, handle, LIST_MATCH_FUNCTION, match_function, const void*, match_context)
        size_t i;
        const void* found_item = NULL;
        for (i = 0; i < list_item_count; i++)
        {
            if (match_function((LIST_ITEM_HANDLE)list_items[i], match_context))
            {
                found_item = list_items[i];
                break;
            }
        }
    MOCK_METHOD_END(LIST_ITEM_HANDLE, (LIST_ITEM_HANDLE)found_item);
    MOCK_STATIC_METHOD_3(, int, list_remove_matching_item, LIST_HANDLE, handle, LIST_MATCH_FUNCTION, match_function, const void*, match_context)
        size_t i;
        int res = __LINE__;
        for (i = 0; i < list_item_count; i++)
        {
            if (match_function((LIST_ITEM_HANDLE)list_items[i], match_context))
            {
                (void)memcpy(&list_items[i], &list_items[i + 1], (list_item_count - i - 1) * sizeof(const void*));
                list_item_count--;
                res = 0;
                break;
            }
        }
    MOCK_METHOD_END(int, res);

    // Lock Mocks 
    MOCK_STATIC_METHOD_0(, LOCK_HANDLE, Lock_Init)
    MOCK_METHOD_END(LOCK_HANDLE, TEST_LOCK_HANDLE);
    MOCK_STATIC_METHOD_1(, LOCK_RESULT, Lock_Deinit, LOCK_HANDLE, handle)
    MOCK_METHOD_END(LOCK_RESULT, LOCK_OK);
    MOCK_STATIC_METHOD_1(, LOCK_RESULT, Lock, LOCK_HANDLE, handle)
    MOCK_METHOD_END(LOCK_RESULT, LOCK_OK);
    MOCK_STATIC_METHOD_1(, LOCK_RESULT, Unlock, LOCK_HANDLE, handle)
    MOCK_METHOD_END(LOCK_RESULT, LOCK_OK);

    // Condition Mocks 
    MOCK_STATIC_METHOD_0(, COND_HANDLE, Condition_Init)
    MOCK_METHOD_END(COND_HANDLE, TEST_COND_HANDLE);
    MOCK_STATIC_METHOD_1(, void, Condition_Deinit, COND_HANDLE, handle)
    MOCK_VOID_METHOD_END();
    MOCK_STATIC_METHOD_1(, COND_RESULT, Condition_Post, COND_HANDLE, handle)
    MOCK_METHOD_END(COND_RESULT, COND_OK);
    MOCK_STATIC_METHOD_3(, COND_RESULT, Condition_Wait, COND_HANDLE, handle, LOCK_HANDLE, lock, int, timeout)
    MOCK_METHOD_END(COND_RESULT, COND_OK);

    // winhttp mocks
    MOCK_STATIC_METHOD_4(, WINHTTP_STATUS_CALLBACK, WinHttpSetStatusCallback, HINTERNET, hInternet, WINHTTP_STATUS_CALLBACK, lpfnInternetCallback, DWORD, dwNotificationFlags, DWORD_PTR, dwReserved)
    MOCK_METHOD_END(WINHTTP_STATUS_CALLBACK, NULL);
    MOCK_STATIC_METHOD_5(, HINTERNET, WinHttpOpen, LPCWSTR, pszAgentW, DWORD, dwAccessType, LPCWSTR, pszProxyW, LPCWSTR, pszProxyBypassW, DWORD, dwFlags)
    MOCK_METHOD_END(HINTERNET, TEST_HOPEN);
    MOCK_STATIC_METHOD_1(, BOOL, WinHttpCloseHandle, HINTERNET, hInternet)
    MOCK_METHOD_END(BOOL, TRUE);
    MOCK_STATIC_METHOD_4(, HINTERNET, WinHttpConnect, HINTERNET, hSession, LPCWSTR, pswzServerName, INTERNET_PORT, nServerPort, DWORD, dwReserved)
    MOCK_METHOD_END(HINTERNET, TEST_HCONNECT);
    MOCK_STATIC_METHOD_7(, HINTERNET, WinHttpOpenRequest, HINTERNET, hConnect, LPCWSTR, pwszVerb, LPCWSTR, pwszObjectName, LPCWSTR, pwszVersion, LPCWSTR, pwszReferrer, LPCWSTR*, ppwszAcceptTypes, DWORD, dwFlags)
    MOCK_METHOD_END(HINTERNET, TEST_HREQUEST);
    MOCK_STATIC_METHOD_4(, BOOL, WinHttpSetOption, HINTERNET, hInternet, DWORD, dwOption, LPVOID, lpBuffer, DWORD, dwBufferLength)
    MOCK_METHOD_END(BOOL, TRUE);
    MOCK_STATIC_METHOD_4(, BOOL, WinHttpAddRequestHeaders, HINTERNET, hRequest, LPCWSTR, lpszHeaders, DWORD, dwHeadersLength, DWORD, dwModifiers)
    MOCK_METHOD_END(BOOL, TRUE);
    MOCK_STATIC_METHOD_7(, BOOL, WinHttpSendRequest, HINTERNET, hRequest, LPCWSTR, lpszHeaders, DWORD, dwHeadersLength, LPVOID, lpOptional, DWORD, dwOptionalLength, DWORD, dwTotalLength, DWORD_PTR, dwContext)
    MOCK_METHOD_END(BOOL, TRUE);
    MOCK_STATIC_METHOD_2(, BOOL, WinHttpReceiveResponse, HINTERNET, hRequest, LPVOID, lpReserved)
    MOCK_METHOD_END(BOOL, TRUE);
    MOCK_STATIC_METHOD_6(, BOOL, WinHttpQueryHeaders, HINTERNET, hRequest, DWORD, dwInfoLevel, LPCWSTR, pwszName, LPVOID, lpBuffer, LPDWORD, lpdwBufferLength, LPDWORD, lpdwIndex)
    MOCK_METHOD_END(BOOL, TRUE);
    MOCK_STATIC_METHOD_2(, HINTERNET, WinHttpWebSocketCompleteUpgrade, HINTERNET, hRequest, DWORD_PTR, pContext)
    MOCK_METHOD_END(HINTERNET, TEST_HWEBSOCKET);
    MOCK_STATIC_METHOD_4(, DWORD, WinHttpWebSocketSend, HINTERNET, hWebSocket, WINHTTP_WEB_SOCKET_BUFFER_TYPE, eBufferType, PVOID, pvBuffer, DWORD, dwBufferLength)
    MOCK_METHOD_END(DWORD, 0);
    MOCK_STATIC_METHOD_5(, DWORD, WinHttpWebSocketReceive, HINTERNET, hWebSocket, PVOID, pvBuffer, DWORD, dwBufferLength, DWORD*, pdwBytesRead, WINHTTP_WEB_SOCKET_BUFFER_TYPE*, peBufferType)
    MOCK_METHOD_END(DWORD, 0);
    MOCK_STATIC_METHOD_4(, DWORD, WinHttpWebSocketClose, HINTERNET, hWebSocket, USHORT, usStatus, PVOID, pvReason, DWORD, dwReasonLength)
    MOCK_METHOD_END(DWORD, 0);

    // consumer mocks
    MOCK_STATIC_METHOD_2(, void, test_on_io_open_complete, void*, context, IO_OPEN_RESULT, io_open_result);
    MOCK_VOID_METHOD_END()
    MOCK_STATIC_METHOD_3(, void, test_on_bytes_received, void*, context, const unsigned char*, buffer, size_t, size);
    MOCK_VOID_METHOD_END()
    MOCK_STATIC_METHOD_1(, void, test_on_io_error, void*, context);
    MOCK_VOID_METHOD_END()
    MOCK_STATIC_METHOD_1(, void, test_on_io_close_complete, void*, context);
    MOCK_VOID_METHOD_END()
    MOCK_STATIC_METHOD_2(, void, test_on_send_complete, void*, context, IO_SEND_RESULT, send_result)
    MOCK_VOID_METHOD_END()
};

extern "C"
{
	DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , void*, amqpalloc_malloc, size_t, size);
	DECLARE_GLOBAL_MOCK_METHOD_2(wsio_win32_mocks, , void*, amqpalloc_realloc, void*, ptr, size_t, size);
	DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , void, amqpalloc_free, void*, ptr);

    DECLARE_GLOBAL_MOCK_METHOD_0(wsio_win32_mocks, , LIST_HANDLE, list_create);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , void, list_destroy, LIST_HANDLE, list);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , LIST_ITEM_HANDLE, list_get_head_item, LIST_HANDLE, list);
    DECLARE_GLOBAL_MOCK_METHOD_2(wsio_win32_mocks, , int, list_remove, LIST_HANDLE, list, LIST_ITEM_HANDLE, item);
    DECLARE_GLOBAL_MOCK_METHOD_2(wsio_win32_mocks, , LIST_ITEM_HANDLE, list_add, LIST_HANDLE, list, const void*, item);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , const void*, list_item_get_value, LIST_ITEM_HANDLE, item_handle);
    DECLARE_GLOBAL_MOCK_METHOD_3(wsio_win32_mocks, , LIST_ITEM_HANDLE, list_find, LIST_HANDLE, handle, LIST_MATCH_FUNCTION, match_function, const void*, match_context);
    DECLARE_GLOBAL_MOCK_METHOD_3(wsio_win32_mocks, , int, list_remove_matching_item, LIST_HANDLE, handle, LIST_MATCH_FUNCTION, match_function, const void*, match_context);

    DECLARE_GLOBAL_MOCK_METHOD_2(wsio_win32_mocks, , void, test_on_io_open_complete, void*, context, IO_OPEN_RESULT, io_open_result);
    DECLARE_GLOBAL_MOCK_METHOD_3(wsio_win32_mocks, , void, test_on_bytes_received, void*, context, const unsigned char*, buffer, size_t, size);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , void, test_on_io_error, void*, context);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , void, test_on_io_close_complete, void*, context);
    DECLARE_GLOBAL_MOCK_METHOD_2(wsio_win32_mocks, , void, test_on_send_complete, void*, context, IO_SEND_RESULT, send_result);
    
    DECLARE_GLOBAL_MOCK_METHOD_4(wsio_win32_mocks, , WINHTTP_STATUS_CALLBACK, WinHttpSetStatusCallback, HINTERNET, hInternet, WINHTTP_STATUS_CALLBACK, lpfnInternetCallback, DWORD, dwNotificationFlags, DWORD_PTR, dwReserved);
    DECLARE_GLOBAL_MOCK_METHOD_5(wsio_win32_mocks, , HINTERNET, WinHttpOpen, LPCWSTR, pszAgentW, DWORD, dwAccessType, LPCWSTR, pszProxyW, LPCWSTR, pszProxyBypassW, DWORD, dwFlags);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , BOOL, WinHttpCloseHandle, HINTERNET, hInternet);
    DECLARE_GLOBAL_MOCK_METHOD_4(wsio_win32_mocks, , HINTERNET, WinHttpConnect, HINTERNET, hSession, LPCWSTR, pswzServerName, INTERNET_PORT, nServerPort, DWORD, dwReserved);
    DECLARE_GLOBAL_MOCK_METHOD_7(wsio_win32_mocks, , HINTERNET, WinHttpOpenRequest, HINTERNET, hConnect, LPCWSTR, pwszVerb, LPCWSTR, pwszObjectName, LPCWSTR, pwszVersion, LPCWSTR, pwszReferrer, LPCWSTR*, ppwszAcceptTypes, DWORD, dwFlags);
    DECLARE_GLOBAL_MOCK_METHOD_4(wsio_win32_mocks, , BOOL, WinHttpSetOption, HINTERNET, hInternet, DWORD, dwOption, LPVOID, lpBuffer, DWORD, dwBufferLength);
    DECLARE_GLOBAL_MOCK_METHOD_4(wsio_win32_mocks, , BOOL, WinHttpAddRequestHeaders, HINTERNET, hRequest, LPCWSTR, lpszHeaders, DWORD, dwHeadersLength, DWORD, dwModifiers);
    DECLARE_GLOBAL_MOCK_METHOD_7(wsio_win32_mocks, , BOOL, WinHttpSendRequest, HINTERNET, hRequest, LPCWSTR, lpszHeaders, DWORD, dwHeadersLength, LPVOID, lpOptional, DWORD, dwOptionalLength, DWORD, dwTotalLength, DWORD_PTR, dwContext);
    DECLARE_GLOBAL_MOCK_METHOD_2(wsio_win32_mocks, , BOOL, WinHttpReceiveResponse, HINTERNET, hRequest, LPVOID, lpReserved);
    DECLARE_GLOBAL_MOCK_METHOD_6(wsio_win32_mocks, , BOOL, WinHttpQueryHeaders, HINTERNET, hRequest, DWORD, dwInfoLevel, LPCWSTR, pwszName, LPVOID, lpBuffer, LPDWORD, lpdwBufferLength, LPDWORD, lpdwIndex);
    DECLARE_GLOBAL_MOCK_METHOD_2(wsio_win32_mocks, , HINTERNET, WinHttpWebSocketCompleteUpgrade, HINTERNET, hRequest, DWORD_PTR, pContext);
    DECLARE_GLOBAL_MOCK_METHOD_4(wsio_win32_mocks, , DWORD, WinHttpWebSocketSend, HINTERNET, hWebSocket, WINHTTP_WEB_SOCKET_BUFFER_TYPE, eBufferType, PVOID, pvBuffer, DWORD, dwBufferLength);
    DECLARE_GLOBAL_MOCK_METHOD_5(wsio_win32_mocks, , DWORD, WinHttpWebSocketReceive, HINTERNET, hWebSocket, PVOID, pvBuffer, DWORD, dwBufferLength, DWORD*, pdwBytesRead, WINHTTP_WEB_SOCKET_BUFFER_TYPE*, peBufferType);
    DECLARE_GLOBAL_MOCK_METHOD_4(wsio_win32_mocks, , DWORD, WinHttpWebSocketClose, HINTERNET, hWebSocket, USHORT, usStatus, PVOID, pvReason, DWORD, dwReasonLength);

    DECLARE_GLOBAL_MOCK_METHOD_0(wsio_win32_mocks, , LOCK_HANDLE, Lock_Init);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , LOCK_RESULT, Lock_Deinit, LOCK_HANDLE, handle);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , LOCK_RESULT, Lock, LOCK_HANDLE, handle);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , LOCK_RESULT, Unlock, LOCK_HANDLE, handle);

    DECLARE_GLOBAL_MOCK_METHOD_0(wsio_win32_mocks, , COND_HANDLE, Condition_Init);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , void, Condition_Deinit, COND_HANDLE, handle);
    DECLARE_GLOBAL_MOCK_METHOD_1(wsio_win32_mocks, , COND_RESULT, Condition_Post, COND_HANDLE, handle);
    DECLARE_GLOBAL_MOCK_METHOD_3(wsio_win32_mocks, , COND_RESULT, Condition_Wait, COND_HANDLE, handle, LOCK_HANDLE, lock, int, timeout);

    extern void test_logger_log(unsigned int options, char* format, ...)
	{
        (void)options;
		(void)format;
	}
}

MICROMOCK_MUTEX_HANDLE test_serialize_mutex;

BEGIN_TEST_SUITE(wsio_win32_unittests)

TEST_SUITE_INITIALIZE(suite_init)
{
	test_serialize_mutex = MicroMockCreateMutex();
	ASSERT_IS_NOT_NULL(test_serialize_mutex);
}

TEST_SUITE_CLEANUP(suite_cleanup)
{
	MicroMockDestroyMutex(test_serialize_mutex);
}

TEST_FUNCTION_INITIALIZE(method_init)
{
	if (!MicroMockAcquireMutex(test_serialize_mutex))
	{
		ASSERT_FAIL("Could not acquire test serialization mutex.");
	}
}

TEST_FUNCTION_CLEANUP(method_cleanup)
{
    free(list_items);
    list_items = NULL;
    list_item_count = 0;
	if (!MicroMockReleaseMutex(test_serialize_mutex))
	{
		ASSERT_FAIL("Could not release test serialization mutex.");
	}
}

/* wsio_create */

/* Tests_SRS_WSIO_01_002: [If the argument io_create_parameters is NULL then wsio_create shall return NULL.] */
TEST_FUNCTION(wsio_win32_create_with_NULL_io_create_parameters_fails)
{
    // arrange
    wsio_win32_mocks mocks;

    // act
    CONCRETE_IO_HANDLE wsio = wsio_create(NULL, test_logger_log);

    // assert
    ASSERT_IS_NULL(wsio);
}

/* Tests_SRS_WSIO_01_004: [If any of the WSIO_CONFIG fields host, protocol_name or relative_path is NULL then wsio_create shall return NULL.] */
TEST_FUNCTION(wsio_win32_create_with_NULL_hostname_fails)
{
    // arrange
    wsio_win32_mocks mocks;
    static WSIO_CONFIG test_wsio_win32_config =
    {
        NULL,
        443,
        "test_ws_protocol",
        "a/b/c",
        false,
        "my_trusted_ca_payload"
    };

    // act
    CONCRETE_IO_HANDLE wsio = wsio_create(&test_wsio_win32_config, test_logger_log);

    // assert
    ASSERT_IS_NULL(wsio);
}

/* Tests_SRS_WSIO_01_004: [If any of the WSIO_CONFIG fields host, protocol_name or relative_path is NULL then wsio_create shall return NULL.] */
TEST_FUNCTION(wsio_win32_create_with_NULL_protocol_name_fails)
{
    // arrange
    wsio_win32_mocks mocks;
    static WSIO_CONFIG test_wsio_win32_config =
    {
        "testhost",
        443,
        NULL,
        "a/b/c",
        false,
        "my_trusted_ca_payload"
    };

    // act
    CONCRETE_IO_HANDLE wsio = wsio_create(&test_wsio_win32_config, test_logger_log);

    // assert
    ASSERT_IS_NULL(wsio);
}

/* Tests_SRS_WSIO_01_004: [If any of the WSIO_CONFIG fields host, protocol_name or relative_path is NULL then wsio_create shall return NULL.] */
TEST_FUNCTION(wsio_win32_create_with_NULL_relative_path_fails)
{
    // arrange
    wsio_win32_mocks mocks;
    static WSIO_CONFIG test_wsio_win32_config =
    {
        "testhost",
        443,
        "test_ws_protocol",
        NULL,
        false,
        "my_trusted_ca_payload"
    };

    // act
    CONCRETE_IO_HANDLE wsio = wsio_create(&test_wsio_win32_config, test_logger_log);

    // assert
    ASSERT_IS_NULL(wsio);
}

/* Tests_SRS_WSIO_01_005: [If allocating memory for the new wsio instance fails then wsio_create shall return NULL.] */
TEST_FUNCTION(when_allocating_memory_for_the_instance_fails_wsio_win32_create_fails)
{
    // arrange
    wsio_win32_mocks mocks;

    EXPECTED_CALL(mocks, amqpalloc_malloc(IGNORED_NUM_ARG))
        .SetReturn((void*)NULL);

    // act
    CONCRETE_IO_HANDLE wsio = wsio_create(&default_wsio_win32_config, test_logger_log);

    // assert
    ASSERT_IS_NULL(wsio);
}

/* wsio_destroy */

/* Tests_SRS_WSIO_01_008: [If ws_io is NULL, wsio_destroy shall do nothing.] */
TEST_FUNCTION(wsio_win32_destroy_with_NULL_handle_does_nothing)
{
    // arrange
    wsio_win32_mocks mocks;

    // act
    wsio_destroy(NULL);

    // assert
    // no explicit assert, uMock checks the calls
}

/* wsio_open */

/* Tests_SRS_WSIO_01_042: [if ws_io is NULL, wsio_close shall return a non-zero value.] */
TEST_FUNCTION(wsio_win32_close_with_NULL_handle_fails)
{
    // arrange
    wsio_win32_mocks mocks;

    // act
    int result = wsio_close(NULL, test_on_io_close_complete, (void*)0x4242);

    // assert
    ASSERT_ARE_NOT_EQUAL(int, 0, result);
}

/* Tests_SRS_WSIO_01_045: [wsio_close when no open action has been issued shall fail and return a non-zero value.] */
TEST_FUNCTION(wsio_win32_close_when_not_open_fails)
{
    // arrange
    wsio_win32_mocks mocks;
    CONCRETE_IO_HANDLE wsio = wsio_create(&default_wsio_win32_config, test_logger_log);
    mocks.ResetAllCalls();

    // act
    int result = wsio_close(wsio, test_on_io_close_complete, (void*)0x4242);

    // assert
    mocks.AssertActualAndExpectedCalls();
    ASSERT_ARE_NOT_EQUAL(int, 0, result);

    // cleanup
    wsio_destroy(wsio);
}

/* Tests_SRS_WSIO_01_046: [wsio_close after a wsio_close shall fail and return a non-zero value.]  */
TEST_FUNCTION(wsio_win32_close_when_already_closed_fails)
{
    // arrange
    wsio_win32_mocks mocks;
    CONCRETE_IO_HANDLE wsio = wsio_create(&default_wsio_win32_config, test_logger_log);
    (void)wsio_open(wsio, test_on_io_open_complete, (void*)0x4242, test_on_bytes_received, (void*)0x4242, test_on_io_error, (void*)0x4242);
    (void)wsio_close(wsio, test_on_io_close_complete, (void*)0x4242);
    mocks.ResetAllCalls();

    // act
    int result = wsio_close(wsio, test_on_io_close_complete, (void*)0x4242);

    // assert
    mocks.AssertActualAndExpectedCalls();
    ASSERT_ARE_NOT_EQUAL(int, 0, result);

    // cleanup
    wsio_destroy(wsio);
}

/* wsio_send */

/* Tests_SRS_WSIO_01_051: [If the wsio is not OPEN (open has not been called or is still in progress) then wsio_send shall fail and return a non-zero value.] */
TEST_FUNCTION(when_ws_io_is_not_opened_yet_wsio_win32_send_fails)
{
    // arrange
    wsio_win32_mocks mocks;
    unsigned char test_buffer[] = { 0x42 };
    CONCRETE_IO_HANDLE wsio = wsio_create(&default_wsio_win32_config, test_logger_log);
    mocks.ResetAllCalls();

    // act
    int result = wsio_send(wsio, test_buffer, sizeof(test_buffer), test_on_send_complete, (void*)0x4243);

    // assert
    mocks.AssertActualAndExpectedCalls();
    ASSERT_ARE_NOT_EQUAL(int, 0, result);

    // cleanup
    wsio_destroy(wsio);
}

/* Tests_SRS_WSIO_01_051: [If the wsio is not OPEN (open has not been called or is still in progress) then wsio_send shall fail and return a non-zero value.] */
TEST_FUNCTION(when_ws_io_is_opening_wsio_win32_send_fails)
{
    // arrange
    wsio_win32_mocks mocks;
    unsigned char test_buffer[] = { 0x42 };
    CONCRETE_IO_HANDLE wsio = wsio_create(&default_wsio_win32_config, test_logger_log);
    (void)wsio_open(wsio, test_on_io_open_complete, (void*)0x4242, test_on_bytes_received, (void*)0x4242, test_on_io_error, (void*)0x4242);
    mocks.ResetAllCalls();

    // act
    int result = wsio_send(wsio, test_buffer, sizeof(test_buffer), test_on_send_complete, (void*)0x4243);

    // assert
    mocks.AssertActualAndExpectedCalls();
    ASSERT_ARE_NOT_EQUAL(int, 0, result);

    // cleanup
    wsio_destroy(wsio);
}

/* Tests_SRS_WSIO_01_052: [If any of the arguments ws_io or buffer are NULL, wsio_send shall fail and return a non-zero value.] */
TEST_FUNCTION(when_wsio_win32_is_NULL_wsio_win32_send_fails)
{
    // arrange
    wsio_win32_mocks mocks;
    unsigned char test_buffer[] = { 0x42 };
    CONCRETE_IO_HANDLE wsio = wsio_create(&default_wsio_win32_config, test_logger_log);
    (void)wsio_open(wsio, test_on_io_open_complete, (void*)0x4242, test_on_bytes_received, (void*)0x4242, test_on_io_error, (void*)0x4242);
    mocks.ResetAllCalls();

    // act
    int result = wsio_send(NULL, test_buffer, sizeof(test_buffer), test_on_send_complete, (void*)0x4243);

    // assert
    mocks.AssertActualAndExpectedCalls();
    ASSERT_ARE_NOT_EQUAL(int, 0, result);

    // cleanup
    wsio_destroy(wsio);
}

/* Tests_SRS_WSIO_01_052: [If any of the arguments ws_io or buffer are NULL, wsio_send shall fail and return a non-zero value.] */
TEST_FUNCTION(when_buffer_is_NULL_wsio_win32_send_fails)
{
    // arrange
    wsio_win32_mocks mocks;
    unsigned char test_buffer[] = { 0x42 };
    CONCRETE_IO_HANDLE wsio = wsio_create(&default_wsio_win32_config, test_logger_log);
    (void)wsio_open(wsio, test_on_io_open_complete, (void*)0x4242, test_on_bytes_received, (void*)0x4242, test_on_io_error, (void*)0x4242);
    mocks.ResetAllCalls();

    // act
    int result = wsio_send(wsio, NULL, sizeof(test_buffer), test_on_send_complete, (void*)0x4243);

    // assert
    mocks.AssertActualAndExpectedCalls();
    ASSERT_ARE_NOT_EQUAL(int, 0, result);

    // cleanup
    wsio_destroy(wsio);
}

/* Tests_SRS_WSIO_01_053: [If size is zero then wsio_send shall fail and return a non-zero value.] */
TEST_FUNCTION(when_size_is_zero_wsio_win32_send_fails)
{
    // arrange
    wsio_win32_mocks mocks;
    unsigned char test_buffer[] = { 0x42 };
    CONCRETE_IO_HANDLE wsio = wsio_create(&default_wsio_win32_config, test_logger_log);
    (void)wsio_open(wsio, test_on_io_open_complete, (void*)0x4242, test_on_bytes_received, (void*)0x4242, test_on_io_error, (void*)0x4242);
    mocks.ResetAllCalls();

    // act
    int result = wsio_send(wsio, test_buffer, 0, test_on_send_complete, (void*)0x4243);

    // assert
    mocks.AssertActualAndExpectedCalls();
    ASSERT_ARE_NOT_EQUAL(int, 0, result);

    // cleanup
    wsio_destroy(wsio);
}

/* Tests_SRS_WSIO_01_063: [If the ws_io argument is NULL, wsio_dowork shall do nothing.]  */
TEST_FUNCTION(wsio_win32_dowork_with_NULL_handles_does_nothing)
{
    // arrange
    wsio_win32_mocks mocks;

    // act
    wsio_dowork(NULL);

    // assert
    // no explicit assert, uMock checks the calls
}

/* Tests_SRS_WSIO_01_062: [This shall be done if the IO is not closed.] */
TEST_FUNCTION(wsio_win32_dowork_does_not_work_when_not_yet_open)
{
    // arrange
    wsio_win32_mocks mocks;
    CONCRETE_IO_HANDLE wsio = wsio_create(&default_wsio_win32_config, test_logger_log);
    mocks.ResetAllCalls();

    // act
    wsio_dowork(wsio);

    // assert
    mocks.AssertActualAndExpectedCalls();

    // cleanup
    wsio_destroy(wsio);
}

/* Tests_SRS_WSIO_01_062: [This shall be done if the IO is not closed.] */
TEST_FUNCTION(wsio_win32_dowork_does_not_work_when_closed)
{
    // arrange
    wsio_win32_mocks mocks;
    CONCRETE_IO_HANDLE wsio = wsio_create(&default_wsio_win32_config, test_logger_log);
    (void)wsio_open(wsio, test_on_io_open_complete, (void*)0x4242, test_on_bytes_received, (void*)0x4242, test_on_io_error, (void*)0x4242);
    wsio_close(wsio, NULL, NULL);
    mocks.ResetAllCalls();

    // act
    wsio_dowork(wsio);

    // assert
    mocks.AssertActualAndExpectedCalls();

    // cleanup
    wsio_destroy(wsio);
}

/* wsio_get_interface_description */

/* Tests_SRS_WSIO_01_064: [wsio_get_interface_description shall return a pointer to an IO_INTERFACE_DESCRIPTION structure that contains pointers to the functions: wsio_create, wsio_destroy, wsio_open, wsio_close, wsio_send and wsio_dowork.] */
TEST_FUNCTION(wsio_win32_get_interface_description_fills_the_interface_structure)
{
    // arrange
    wsio_win32_mocks mocks;

    // act
    const IO_INTERFACE_DESCRIPTION* if_description = wsio_get_interface_description();

    // assert
    ASSERT_ARE_EQUAL(void_ptr, wsio_create, if_description->concrete_io_create);
    ASSERT_ARE_EQUAL(void_ptr, wsio_destroy, if_description->concrete_io_destroy);
    ASSERT_ARE_EQUAL(void_ptr, wsio_open, if_description->concrete_io_open);
    ASSERT_ARE_EQUAL(void_ptr, wsio_close, if_description->concrete_io_close);
    ASSERT_ARE_EQUAL(void_ptr, wsio_send, if_description->concrete_io_send);
    ASSERT_ARE_EQUAL(void_ptr, wsio_dowork, if_description->concrete_io_dowork);
    ASSERT_ARE_EQUAL(void_ptr, wsio_setoption, if_description->concrete_io_setoption);
}

END_TEST_SUITE(wsio_win32_unittests)
