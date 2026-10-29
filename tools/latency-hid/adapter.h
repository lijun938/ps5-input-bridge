/* Declared before including production ps5bridge.c so only its HID I/O calls
 * are redirected. No real controller is opened. */
#include <winsock2.h>
#include <windows.h>
#include <hidsdi.h>
#include <chiaki/session.h>
static HANDLE WINAPI bench_open(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static BOOL WINAPI bench_read(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL WINAPI bench_result(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
static BOOL WINAPI bench_cancel(HANDLE, LPOVERLAPPED);
static BOOLEAN WINAPI bench_set_buffers(HANDLE, ULONG);
static BOOLEAN WINAPI bench_get_buffers(HANDLE, PULONG);
static BOOLEAN WINAPI bench_feature(HANDLE, PVOID, ULONG);
ChiakiErrorCode bench_submit(ChiakiSession *, ChiakiControllerState *);
#define CreateFileW bench_open
#define ReadFile bench_read
#define GetOverlappedResult bench_result
#define CancelIoEx bench_cancel
#define HidD_SetNumInputBuffers bench_set_buffers
#define HidD_GetNumInputBuffers bench_get_buffers
#define HidD_GetFeature bench_feature
#define chiaki_session_set_controller_state bench_submit
