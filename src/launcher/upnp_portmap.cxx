#include "upnp_portmap.hxx"

#include "../common/sf4e__NetUtil.hxx"
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <comdef.h>
#include <netcon.h>
#include <natupnp.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace sf4e {
namespace launcher {

	static bool MapOne(IStaticPortMappingCollection* mappings, long externalPort, long internalPort, const char* protocol, const char* clientIp, const char* description) {
		if (!mappings || !protocol || !clientIp) {
			return false;
		}
		BSTR bstrProtocol = SysAllocString(protocol[0] == 'U' || protocol[0] == 'u' ? L"UDP" : L"TCP");
		BSTR bstrClient = SysAllocStringLen(NULL, 0);
		int wideLen = MultiByteToWideChar(CP_UTF8, 0, clientIp, -1, NULL, 0);
		if (wideLen > 0) {
			bstrClient = SysAllocStringLen(NULL, wideLen);
			MultiByteToWideChar(CP_UTF8, 0, clientIp, -1, bstrClient, wideLen);
		}
		BSTR bstrDesc = SysAllocString(L"SF4 Enhanced");
		if (description && description[0]) {
			int dlen = MultiByteToWideChar(CP_UTF8, 0, description, -1, NULL, 0);
			if (dlen > 0) {
				SysFreeString(bstrDesc);
				bstrDesc = SysAllocStringLen(NULL, dlen);
				MultiByteToWideChar(CP_UTF8, 0, description, -1, bstrDesc, dlen);
			}
		}

		IStaticPortMapping* mapping = NULL;
		HRESULT hr = mappings->Add(
			externalPort,
			bstrProtocol,
			internalPort,
			bstrClient,
			VARIANT_TRUE,
			bstrDesc,
			&mapping
		);
		if (mapping) {
			mapping->Release();
		}
		SysFreeString(bstrProtocol);
		SysFreeString(bstrClient);
		SysFreeString(bstrDesc);
		return SUCCEEDED(hr);
	}

	bool TryMapUpnpPort(uint16_t externalPort, uint16_t internalPort, const char* protocol, const char* description) {
		char lanIp[64] = { 0 };
		if (!sf4e::DetectLanIPv4(lanIp, sizeof(lanIp))) {
			return false;
		}

		HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
		bool coInit = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

		IUPnPNAT* nat = NULL;
		hr = CoCreateInstance(CLSID_UPnPNAT, NULL, CLSCTX_INPROC_SERVER, IID_IUPnPNAT, (void**)&nat);
		if (FAILED(hr) || !nat) {
			if (coInit && hr != RPC_E_CHANGED_MODE) {
				CoUninitialize();
			}
			return false;
		}

		IStaticPortMappingCollection* mappings = NULL;
		hr = nat->get_StaticPortMappingCollection(&mappings);
		bool ok = false;
		if (SUCCEEDED(hr) && mappings) {
			ok = MapOne(mappings, externalPort, internalPort, protocol, lanIp, description);
			mappings->Release();
		}
		nat->Release();
		if (coInit && hr != RPC_E_CHANGED_MODE) {
			CoUninitialize();
		}
		return ok;
	}

} // namespace launcher
} // namespace sf4e
