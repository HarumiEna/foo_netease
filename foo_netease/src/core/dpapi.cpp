#include "core/dpapi.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstdio>

#pragma comment(lib, "crypt32.lib")

namespace netease {

namespace {

std::string last_error_text(const char * what) {
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%s 失败：Win32 错误 %lu", what, GetLastError());
	return buf;
}

const char kDescription[] = "foo_netease credentials";

} // namespace

bool dpapi_protect(const std::string & plaintext, Bytes & ciphertext, std::string * error) {
	ciphertext.clear();
	DATA_BLOB in{};
	in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(plaintext.data()));
	in.cbData = static_cast<DWORD>(plaintext.size());

	DATA_BLOB out{};
	const std::string desc(kDescription);
	if (!CryptProtectData(&in, L"foo_netease", nullptr, nullptr, nullptr,
			CRYPTPROTECT_UI_FORBIDDEN, &out)) {
		if (error) *error = last_error_text("CryptProtectData");
		return false;
	}
	ciphertext.assign(out.pbData, out.pbData + out.cbData);
	LocalFree(out.pbData);
	return true;
}

bool dpapi_unprotect(const Bytes & ciphertext, std::string & plaintext, std::string * error) {
	plaintext.clear();
	if (ciphertext.empty()) {
		if (error) *error = "密文为空";
		return false;
	}
	DATA_BLOB in{};
	in.pbData = const_cast<BYTE *>(ciphertext.data());
	in.cbData = static_cast<DWORD>(ciphertext.size());

	DATA_BLOB out{};
	LPWSTR desc = nullptr;
	if (!CryptUnprotectData(&in, &desc, nullptr, nullptr, nullptr,
			CRYPTPROTECT_UI_FORBIDDEN, &out)) {
		if (error) *error = last_error_text("CryptUnprotectData");
		return false;
	}
	if (desc) LocalFree(desc);
	plaintext.assign(reinterpret_cast<char *>(out.pbData), out.cbData);
	SecureZeroMemory(out.pbData, out.cbData);
	LocalFree(out.pbData);
	return true;
}

} // namespace netease

