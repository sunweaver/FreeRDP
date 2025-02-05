/**
 * WinPR: Windows Portable Runtime
 * Process Thread Functions
 *
 * Copyright 2012 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2015 Hewlett-Packard Development Company, L.P.
 * Copyright 2021 David Fort <contact@hardening-consulting.com>
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <winpr/config.h>

#include <winpr/assert.h>

#include <winpr/handle.h>

#include <winpr/thread.h>

/**
 * api-ms-win-core-processthreads-l1-1-1.dll
 *
 * CreateRemoteThread
 * CreateRemoteThreadEx
 * CreateThread
 * DeleteProcThreadAttributeList
 * ExitThread
 * FlushInstructionCache
 * FlushProcessWriteBuffers
 * GetCurrentThread
 * GetCurrentThreadId
 * GetCurrentThreadStackLimits
 * GetExitCodeThread
 * GetPriorityClass
 * GetStartupInfoW
 * GetThreadContext
 * GetThreadId
 * GetThreadIdealProcessorEx
 * GetThreadPriority
 * GetThreadPriorityBoost
 * GetThreadTimes
 * InitializeProcThreadAttributeList
 * OpenThread
 * OpenThreadToken
 * QueryProcessAffinityUpdateMode
 * QueueUserAPC
 * ResumeThread
 * SetPriorityClass
 * SetThreadContext
 * SetThreadPriority
 * SetThreadPriorityBoost
 * SetThreadStackGuarantee
 * SetThreadToken
 * SuspendThread
 * SwitchToThread
 * TerminateThread
 * UpdateProcThreadAttribute
 */

#ifndef _WIN32

#include <winpr/crt.h>
#include <winpr/platform.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_EVENTFD_H
#include <sys/eventfd.h>
#endif

#include <winpr/debug.h>

#include <errno.h>
#include <fcntl.h>

#include <winpr/collections.h>

#include "thread.h"
#include "apc.h"

#include "../handle/handle.h"
#include "../log.h"
#define TAG WINPR_TAG("thread")

static WINPR_THREAD mainThread;

#if defined(WITH_THREAD_LIST)
static wListDictionary* thread_list = NULL;
#endif

static BOOL ThreadCloseHandle(HANDLE handle);
static void cleanup_handle(void* obj);

static BOOL ThreadIsHandled(HANDLE handle)
{
	WINPR_THREAD* pThread = (WINPR_THREAD*)handle;

	if (!pThread || (pThread->Type != HANDLE_TYPE_THREAD))
	{
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	return TRUE;
}

static int ThreadGetFd(HANDLE handle)
{
	WINPR_THREAD* pThread = (WINPR_THREAD*)handle;

	if (!ThreadIsHandled(handle))
		return -1;

	return pThread->event.fds[0];
}

static DWORD ThreadCleanupHandle(HANDLE handle)
{
	WINPR_THREAD* thread = (WINPR_THREAD*)handle;

	if (!ThreadIsHandled(handle))
		return WAIT_FAILED;

	if (pthread_mutex_lock(&thread->mutex))
		return WAIT_FAILED;

	if (!thread->joined)
	{
		int status;
		status = pthread_join(thread->thread, NULL);

		if (status != 0)
		{
			WLog_ERR(TAG, "pthread_join failure: [%d] %s", status, strerror(status));
			pthread_mutex_unlock(&thread->mutex);
			return WAIT_FAILED;
		}
		else
			thread->joined = TRUE;
	}

	if (pthread_mutex_unlock(&thread->mutex))
		return WAIT_FAILED;

	return WAIT_OBJECT_0;
}

static HANDLE_OPS ops = { ThreadIsHandled,
	                      ThreadCloseHandle,
	                      ThreadGetFd,
	                      ThreadCleanupHandle,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL,
	                      NULL };

static void dump_thread(WINPR_THREAD* thread)
{
#if defined(WITH_DEBUG_THREADS)
	void* stack = winpr_backtrace(20);
	char** msg;
	size_t used, i;
	WLog_DBG(TAG, "Called from:");
	msg = winpr_backtrace_symbols(stack, &used);

	for (i = 0; i < used; i++)
		WLog_DBG(TAG, "[%" PRIdz "]: %s", i, msg[i]);

	free(msg);
	winpr_backtrace_free(stack);
	WLog_DBG(TAG, "Thread handle created still not closed!");
	msg = winpr_backtrace_symbols(thread->create_stack, &used);

	for (i = 0; i < used; i++)
		WLog_DBG(TAG, "[%" PRIdz "]: %s", i, msg[i]);

	free(msg);

	if (thread->started)
	{
		WLog_DBG(TAG, "Thread still running!");
	}
	else if (!thread->exit_stack)
	{
		WLog_DBG(TAG, "Thread suspended.");
	}
	else
	{
		WLog_DBG(TAG, "Thread exited at:");
		msg = winpr_backtrace_symbols(thread->exit_stack, &used);

		for (i = 0; i < used; i++)
			WLog_DBG(TAG, "[%" PRIdz "]: %s", i, msg[i]);

		free(msg);
	}
#else
	WINPR_UNUSED(thread);
#endif
}

/**
 * TODO: implement thread suspend/resume using pthreads
 * http://stackoverflow.com/questions/3140867/suspend-pthreads-without-using-condition
 */
static BOOL set_event(WINPR_THREAD* thread)
{
	return winpr_event_set(&thread->event);
}

static BOOL reset_event(WINPR_THREAD* thread)
{
	return winpr_event_reset(&thread->event);
}

#if defined(WITH_THREAD_LIST)
static BOOL thread_compare(const void* a, const void* b)
{
	const pthread_t* p1 = a;
	const pthread_t* p2 = b;
	BOOL rc = pthread_equal(*p1, *p2);
	return rc;
}
#endif

static INIT_ONCE threads_InitOnce = INIT_ONCE_STATIC_INIT;
static pthread_t mainThreadId;
static DWORD currentThreadTlsIndex = TLS_OUT_OF_INDEXES;

static BOOL initializeThreads(PINIT_ONCE InitOnce, PVOID Parameter, PVOID* Context)
{
	if (!apc_init(&mainThread.apc))
	{
		WLog_ERR(TAG, "failed to initialize APC");
		goto out;
	}

	mainThread.Type = HANDLE_TYPE_THREAD;
	mainThreadId = pthread_self();

	currentThreadTlsIndex = TlsAlloc();
	if (currentThreadTlsIndex == TLS_OUT_OF_INDEXES)
	{
		WLog_ERR(TAG, "Major bug, unable to allocate a TLS value for currentThread");
	}

#if defined(WITH_THREAD_LIST)
	thread_list = ListDictionary_New(TRUE);

	if (!thread_list)
	{
		WLog_ERR(TAG, "Couldn't create global thread list");
		goto error_thread_list;
	}

	thread_list->objectKey.fnObjectEquals = thread_compare;
#endif

out:
	return TRUE;
}

/* Thread launcher function responsible for registering
 * cleanup handlers and calling pthread_exit, if not done
 * in thread function. */
static void* thread_launcher(void* arg)
{
	struct timespec waittime = { 1, 0 };
	DWORD rc = 0;
	WINPR_THREAD* thread = (WINPR_THREAD*)arg;
	LPTHREAD_START_ROUTINE fkt;

	if (!thread)
	{
		WLog_ERR(TAG, "Called with invalid argument %p", arg);
		goto exit;
	}

	if (!TlsSetValue(currentThreadTlsIndex, thread))
	{
		WLog_ERR(TAG, "thread %d, unable to set current thread value", pthread_self());
		goto exit;
	}

	if (!(fkt = thread->lpStartAddress))
	{
		WLog_ERR(TAG, "Thread function argument is %p", (void*)fkt);
		goto exit;
	}

	if (pthread_mutex_lock(&thread->threadIsReadyMutex))
		goto exit;

	if (pthread_cond_signal(&thread->threadReady) != 0)
	{
		WLog_ERR(TAG, "The thread could not be made ready");
		pthread_mutex_unlock(&thread->threadIsReadyMutex);
		goto exit;
	}
	pthread_cond_timedwait(&thread->threadIsReady, &thread->threadIsReadyMutex, &waittime);

#if defined(WITH_THREAD_LIST)
	if (!ListDictionary_Contains(thread_list, &thread->thread))
	{
		WLog_ERR(TAG, "Thread not in thread_list, startup failed!");
		pthread_mutex_unlock(&thread->threadIsReadyMutex);
		goto exit;
	}
#endif

	if (pthread_mutex_unlock(&thread->threadIsReadyMutex))
		goto exit;

	rc = fkt(thread->lpParameter);
exit:

	if (thread)
	{
		apc_cleanupThread(thread);

		if (!thread->exited)
			thread->dwExitCode = rc;

		set_event(thread);

		pthread_cond_signal(&thread->threadReady);
		if (thread->detached || !thread->started)
			cleanup_handle(thread);
	}

	return NULL;
}

static BOOL winpr_StartThread(WINPR_THREAD* thread)
{
	struct timespec waittime = { 1, 0 };
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	if (thread->dwStackSize > 0)
		pthread_attr_setstacksize(&attr, (size_t)thread->dwStackSize);

	thread->started = TRUE;
	reset_event(thread);

	if (pthread_mutex_lock(&thread->threadReadyMutex))
		goto error;

#if defined(WITH_THREAD_LIST)
	if (!ListDictionary_Add(thread_list, &thread->thread, thread))
	{
		WLog_ERR(TAG, "failed to add the thread to the thread list");
		pthread_mutex_unlock(&thread->threadReadyMutex);
		goto error;
	}
#endif

	if (pthread_create(&thread->thread, &attr, thread_launcher, thread))
	{
		pthread_mutex_unlock(&thread->threadReadyMutex);
		goto error;
	}

	pthread_cond_timedwait(&thread->threadReady, &thread->threadReadyMutex, &waittime);

	if (pthread_mutex_unlock(&thread->threadReadyMutex))
		goto error;

	if (pthread_cond_signal(&thread->threadIsReady) != 0)
	{
		WLog_ERR(TAG, "failed to signal the thread was ready");
		goto error;
	}

	pthread_attr_destroy(&attr);
	dump_thread(thread);
	return TRUE;
error:
	pthread_attr_destroy(&attr);
	return FALSE;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                    LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter,
                    DWORD dwCreationFlags, LPDWORD lpThreadId)
{
	HANDLE handle;
	WINPR_THREAD* thread = (WINPR_THREAD*)calloc(1, sizeof(WINPR_THREAD));

	if (!thread)
		return NULL;

	thread->dwStackSize = dwStackSize;
	thread->lpParameter = lpParameter;
	thread->lpStartAddress = lpStartAddress;
	thread->lpThreadAttributes = lpThreadAttributes;
	thread->ops = &ops;
#if defined(WITH_DEBUG_THREADS)
	thread->create_stack = winpr_backtrace(20);
	dump_thread(thread);
#endif

	if (!winpr_event_init(&thread->event))
	{
		WLog_ERR(TAG, "failed to create event");
		goto fail;
	}

	if (pthread_mutex_init(&thread->mutex, NULL) != 0)
	{
		WLog_ERR(TAG, "failed to initialize thread mutex");
		goto fail;
	}

	if (!apc_init(&thread->apc))
	{
		WLog_ERR(TAG, "failed to initialize APC");
		goto fail;
	}

	if (pthread_mutex_init(&thread->threadIsReadyMutex, NULL) != 0)
	{
		WLog_ERR(TAG, "failed to initialize a mutex for a condition variable");
		goto fail;
	}

	if (pthread_mutex_init(&thread->threadReadyMutex, NULL) != 0)
	{
		WLog_ERR(TAG, "failed to initialize a mutex for a condition variable");
		goto fail;
	}

	if (pthread_cond_init(&thread->threadIsReady, NULL) != 0)
	{
		WLog_ERR(TAG, "failed to initialize a condition variable");
		goto fail;
	}
	if (pthread_cond_init(&thread->threadReady, NULL) != 0)
	{
		WLog_ERR(TAG, "failed to initialize a condition variable");
		goto fail;
	}

	WINPR_HANDLE_SET_TYPE_AND_MODE(thread, HANDLE_TYPE_THREAD, WINPR_FD_READ);
	handle = (HANDLE)thread;

	InitOnceExecuteOnce(&threads_InitOnce, initializeThreads, NULL, NULL);

	if (!(dwCreationFlags & CREATE_SUSPENDED))
	{
		if (!winpr_StartThread(thread))
			goto fail;
	}
	else
	{
		if (!set_event(thread))
			goto fail;
	}

	return handle;
fail:
	cleanup_handle(thread);
	return NULL;
}

void cleanup_handle(void* obj)
{
	int rc;
	WINPR_THREAD* thread = (WINPR_THREAD*)obj;
	if (!thread)
		return;

	if (!apc_uninit(&thread->apc))
		WLog_ERR(TAG, "failed to destroy APC");

	rc = pthread_cond_destroy(&thread->threadIsReady);
	if (rc)
		WLog_ERR(TAG, "failed to destroy thread->threadIsReady [%d] %s (%d)", rc, strerror(errno),
		         errno);

	rc = pthread_cond_destroy(&thread->threadReady);
	if (rc)
		WLog_ERR(TAG, "failed to destroy thread->threadReady [%d] %s (%d)", rc, strerror(errno),
		         errno);

	rc = pthread_mutex_destroy(&thread->threadIsReadyMutex);
	if (rc)
		WLog_ERR(TAG, "failed to destroy thread->threadIsReadyMutex [%d] %s (%d)", rc,
		         strerror(errno), errno);

	rc = pthread_mutex_destroy(&thread->threadReadyMutex);
	if (rc)
		WLog_ERR(TAG, "failed to destroy thread->threadReadyMutex [%d] %s (%d)", rc,
		         strerror(errno), errno);

	rc = pthread_mutex_destroy(&thread->mutex);
	if (rc)
		WLog_ERR(TAG, "failed to destroy thread->mutex [%d] %s (%d)", rc, strerror(errno), errno);

	winpr_event_uninit(&thread->event);

#if defined(WITH_THREAD_LIST)
	ListDictionary_Remove(thread_list, &thread->thread);
#endif
#if defined(WITH_DEBUG_THREADS)

	if (thread->create_stack)
		winpr_backtrace_free(thread->create_stack);

	if (thread->exit_stack)
		winpr_backtrace_free(thread->exit_stack);

#endif
	free(thread);
}

BOOL ThreadCloseHandle(HANDLE handle)
{
	WINPR_THREAD* thread = (WINPR_THREAD*)handle;

#if defined(WITH_THREAD_LIST)
	if (!thread_list)
	{
		WLog_ERR(TAG, "Thread list does not exist, check call!");
		dump_thread(thread);
	}
	else if (!ListDictionary_Contains(thread_list, &thread->thread))
	{
		WLog_ERR(TAG, "Thread list does not contain this thread! check call!");
		dump_thread(thread);
	}
	else
	{
		ListDictionary_Lock(thread_list);
#endif
		dump_thread(thread);

		if ((thread->started) && (WaitForSingleObject(thread, 0) != WAIT_OBJECT_0))
		{
			WLog_DBG(TAG, "Thread running, setting to detached state!");
			thread->detached = TRUE;
			pthread_detach(thread->thread);
		}
		else
		{
			cleanup_handle(thread);
		}

#if defined(WITH_THREAD_LIST)
		ListDictionary_Unlock(thread_list);
	}
#endif

	return TRUE;
}

HANDLE CreateRemoteThread(HANDLE hProcess, LPSECURITY_ATTRIBUTES lpThreadAttributes,
                          SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
                          LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId)
{
	WLog_ERR(TAG, "%s: not implemented", __FUNCTION__);
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return NULL;
}

VOID ExitThread(DWORD dwExitCode)
{
#if defined(WITH_THREAD_LIST)
	DWORD rc;
	pthread_t tid = pthread_self();

	if (!thread_list)
	{
		WLog_ERR(TAG, "function called without existing thread list!");
#if defined(WITH_DEBUG_THREADS)
		DumpThreadHandles();
#endif
		pthread_exit(0);
	}
	else if (!ListDictionary_Contains(thread_list, &tid))
	{
		WLog_ERR(TAG, "function called, but no matching entry in thread list!");
#if defined(WITH_DEBUG_THREADS)
		DumpThreadHandles();
#endif
		pthread_exit(0);
	}
	else
	{
		WINPR_THREAD* thread;
		ListDictionary_Lock(thread_list);
		thread = ListDictionary_GetItemValue(thread_list, &tid);
		WINPR_ASSERT(thread);
		thread->exited = TRUE;
		thread->dwExitCode = dwExitCode;
#if defined(WITH_DEBUG_THREADS)
		thread->exit_stack = winpr_backtrace(20);
#endif
		ListDictionary_Unlock(thread_list);
		set_event(thread);
		rc = thread->dwExitCode;

		if (thread->detached || !thread->started)
			cleanup_handle(thread);

		pthread_exit((void*)(size_t)rc);
	}
#else
	WINPR_UNUSED(dwExitCode);
#endif
}

BOOL GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode)
{
	ULONG Type;
	WINPR_HANDLE* Object;
	WINPR_THREAD* thread;

	if (!winpr_Handle_GetInfo(hThread, &Type, &Object))
		return FALSE;

	thread = (WINPR_THREAD*)Object;
	*lpExitCode = thread->dwExitCode;
	return TRUE;
}

WINPR_THREAD* winpr_GetCurrentThread(VOID)
{
	WINPR_THREAD* ret;

	InitOnceExecuteOnce(&threads_InitOnce, initializeThreads, NULL, NULL);
	if (mainThreadId == pthread_self())
		return (HANDLE)&mainThread;

	ret = TlsGetValue(currentThreadTlsIndex);
	if (!ret)
	{
		WLog_ERR(TAG, "function called, but no matching entry in thread list!");
#if defined(WITH_DEBUG_THREADS)
		DumpThreadHandles();
#endif
	}
	return ret;
}

HANDLE _GetCurrentThread(VOID)
{
	return (HANDLE)winpr_GetCurrentThread();
}

DWORD GetCurrentThreadId(VOID)
{
	pthread_t tid;
	tid = pthread_self();
	/* Since pthread_t can be 64-bits on some systems, take just the    */
	/* lower 32-bits of it for the thread ID returned by this function. */
	return (DWORD)tid & 0xffffffffUL;
}

typedef struct
{
	WINPR_APC_ITEM apc;
	PAPCFUNC completion;
	ULONG_PTR completionArg;
} UserApcItem;

static void userAPC(LPVOID arg)
{
	UserApcItem* userApc = (UserApcItem*)arg;

	userApc->completion(userApc->completionArg);

	userApc->apc.markedForRemove = TRUE;
}

DWORD QueueUserAPC(PAPCFUNC pfnAPC, HANDLE hThread, ULONG_PTR dwData)
{
	ULONG Type;
	WINPR_HANDLE* Object;
	WINPR_APC_ITEM* apc;
	UserApcItem* apcItem;

	if (!pfnAPC)
		return 1;

	if (!winpr_Handle_GetInfo(hThread, &Type, &Object) || Object->Type != HANDLE_TYPE_THREAD)
	{
		WLog_ERR(TAG, "hThread is not a thread");
		SetLastError(ERROR_INVALID_PARAMETER);
		return (DWORD)0;
	}

	apcItem = calloc(1, sizeof(*apcItem));
	if (!apcItem)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return (DWORD)0;
	}

	apc = &apcItem->apc;
	apc->type = APC_TYPE_USER;
	apc->markedForFree = TRUE;
	apc->alwaysSignaled = TRUE;
	apc->completion = userAPC;
	apc->completionArgs = apc;
	apcItem->completion = pfnAPC;
	apcItem->completionArg = dwData;
	apc_register(hThread, apc);
	return 1;
}

DWORD ResumeThread(HANDLE hThread)
{
	ULONG Type;
	WINPR_HANDLE* Object;
	WINPR_THREAD* thread;

	if (!winpr_Handle_GetInfo(hThread, &Type, &Object))
		return (DWORD)-1;

	thread = (WINPR_THREAD*)Object;

	if (pthread_mutex_lock(&thread->mutex))
		return (DWORD)-1;

	if (!thread->started)
	{
		if (!winpr_StartThread(thread))
		{
			pthread_mutex_unlock(&thread->mutex);
			return (DWORD)-1;
		}
	}
	else
		WLog_WARN(TAG, "Thread already started!");

	if (pthread_mutex_unlock(&thread->mutex))
		return (DWORD)-1;

	return 0;
}

DWORD SuspendThread(HANDLE hThread)
{
	WLog_ERR(TAG, "%s: not implemented", __FUNCTION__);
	SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return (DWORD)-1;
}

BOOL SwitchToThread(VOID)
{
	/**
	 * Note: on some operating systems sched_yield is a stub returning -1.
	 * usleep should at least trigger a context switch if any thread is waiting.
	 */
	if (sched_yield() != 0)
		usleep(1);

	return TRUE;
}

BOOL TerminateThread(HANDLE hThread, DWORD dwExitCode)
{
	ULONG Type;
	WINPR_HANDLE* Object;
	WINPR_THREAD* thread;

	if (!winpr_Handle_GetInfo(hThread, &Type, &Object))
		return FALSE;

	thread = (WINPR_THREAD*)Object;
	thread->exited = TRUE;
	thread->dwExitCode = dwExitCode;

	if (pthread_mutex_lock(&thread->mutex))
		return FALSE;

#ifndef ANDROID
	pthread_cancel(thread->thread);
#else
	WLog_ERR(TAG, "Function not supported on this platform!");
#endif

	if (pthread_mutex_unlock(&thread->mutex))
		return FALSE;

	set_event(thread);
	return TRUE;
}

#if defined(WITH_DEBUG_THREADS)
VOID DumpThreadHandles(void)
{
	char** msg;
	size_t used, i;
	void* stack = winpr_backtrace(20);
	WLog_DBG(TAG, "---------------- Called from ----------------------------");
	msg = winpr_backtrace_symbols(stack, &used);

	for (i = 0; i < used; i++)
	{
		WLog_DBG(TAG, "[%" PRIdz "]: %s", i, msg[i]);
	}

	free(msg);
	winpr_backtrace_free(stack);
	WLog_DBG(TAG, "---------------- Start Dumping thread handles -----------");

#if defined(WITH_THREAD_LIST)
	if (!thread_list)
	{
		WLog_DBG(TAG, "All threads properly shut down and disposed of.");
	}
	else
	{
		ULONG_PTR* keys = NULL;
		ListDictionary_Lock(thread_list);
		int x, count = ListDictionary_GetKeys(thread_list, &keys);
		WLog_DBG(TAG, "Dumping %d elements", count);

		for (x = 0; x < count; x++)
		{
			WINPR_THREAD* thread = ListDictionary_GetItemValue(thread_list, (void*)keys[x]);
			WLog_DBG(TAG, "Thread [%d] handle created still not closed!", x);
			msg = winpr_backtrace_symbols(thread->create_stack, &used);

			for (i = 0; i < used; i++)
			{
				WLog_DBG(TAG, "[%" PRIdz "]: %s", i, msg[i]);
			}

			free(msg);

			if (thread->started)
			{
				WLog_DBG(TAG, "Thread [%d] still running!", x);
			}
			else
			{
				WLog_DBG(TAG, "Thread [%d] exited at:", x);
				msg = winpr_backtrace_symbols(thread->exit_stack, &used);

				for (i = 0; i < used; i++)
					WLog_DBG(TAG, "[%" PRIdz "]: %s", i, msg[i]);

				free(msg);
			}
		}

		free(keys);
		ListDictionary_Unlock(thread_list);
	}
#endif

	WLog_DBG(TAG, "---------------- End Dumping thread handles -------------");
}
#endif
#endif
