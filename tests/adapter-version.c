/* Wine integration regression: the public DXGI version must remain usable by
 * applications that parse its four decimal components. Run with the Neptune
 * DLLs and one Wine display adapter. No game is needed. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dxgi.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

int main(void)
{
   DISPLAY_DEVICEW display = {0};
   display.cb = sizeof(display);
   if (!EnumDisplayDevicesW(NULL, 0, &display, 0)) return 77;

   IDXGIFactory *factory = NULL;
   IDXGIAdapter *adapter = NULL;
   HRESULT hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
   if (FAILED(hr)) return 1;
   hr = IDXGIFactory_EnumAdapters(factory, 0, &adapter);
   if (FAILED(hr)) { IDXGIFactory_Release(factory); return 1; }
   LARGE_INTEGER version = {0};
   hr = IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &version);
   unsigned long long v = (unsigned long long)version.QuadPart;
   char decimal[64];
   snprintf(decimal, sizeof(decimal), "%u%02u%02u%04u",
            (unsigned)(v >> 48), (unsigned)((v >> 32) & 65535),
            (unsigned)((v >> 16) & 65535), (unsigned)(v & 65535));
   errno = 0;
   (void)strtoull(decimal, NULL, 10);
   int overflow = errno == ERANGE;
   int failed = FAILED(hr) || v == ~0ULL || overflow;
   printf("supported hr=%08lx version=%016llx overflow=%d\n",
          (unsigned long)hr, v, overflow);
   hr = IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_ID3D11Device, &version);
   failed |= hr != DXGI_ERROR_UNSUPPORTED;
   printf("unsupported hr=%08lx\n", (unsigned long)hr);
   IDXGIAdapter_Release(adapter);
   IDXGIFactory_Release(factory);
   puts(failed ? "adapter-version: FAIL" : "adapter-version: PASS");
   return failed;
}
