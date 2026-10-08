#include "core/crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

// 常量出处（核对过，不要凭记忆改）：
//
//  · presetKey / iv / RSA 公钥 / eapiKey 取自社区维护的实现
//    https://raw.githubusercontent.com/memo-db/ncm-api/main/util/crypto.js
//    （该文件与 @neteasecloudmusicapienhanced/api 的同名文件一致）。
//
//  · RSA 公钥在那边是 PEM 形式，这里存的是从 PEM 解析出的模数与指数。
//    模数经 Node 的 crypto.createPublicKey(PEM).export({format:'jwk'}) 提取，
//    与 PEM 完全等价，指数为 010001。
//
//  · 结果正确性用固定输入与 Node 对同一输入的输出逐字节比对过。

namespace netease {

namespace {

const char * kWeapiPresetKey = "0CoJUm6Qyw8W8jud";
const char * kWeapiIv = "0102030405060708";
const char * kEapiKey = "e82ckenh8dichen8";

const char * kRsaModulusHex =
	"e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b72515"
	"2b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ec"
	"bda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d8"
	"13cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7";
const char * kRsaExponentHex = "010001";

std::string status_text(NTSTATUS st) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(st));
	return buf;
}

void set_error(std::string * error, const std::string & text) {
	if (error) *error = text;
}

bool failed(std::string * error, const char * what, NTSTATUS st) {
	set_error(error, std::string(what) + " 失败：" + status_text(st));
	return false;
}

bool hex_to_bytes(const std::string & hex, Bytes & out) {
	if (hex.size() % 2 != 0) return false;
	out.clear();
	out.reserve(hex.size() / 2);
	auto nibble = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < hex.size(); i += 2) {
		const int hi = nibble(hex[i]);
		const int lo = nibble(hex[i + 1]);
		if (hi < 0 || lo < 0) return false;
		out.push_back(static_cast<uint8_t>((hi << 4) | lo));
	}
	return true;
}

// 手写 PKCS#7 填充：BCrypt 只做裸的分组加密，填充由调用方负责。
Bytes pkcs7_pad(const std::string & plaintext) {
	const size_t block = 16;
	const size_t pad = block - (plaintext.size() % block);
	Bytes out(plaintext.begin(), plaintext.end());
	out.insert(out.end(), pad, static_cast<uint8_t>(pad));
	return out;
}

class AlgHandle {
public:
	explicit AlgHandle(LPCWSTR alg) { m_status = BCryptOpenAlgorithmProvider(&m_h, alg, nullptr, 0); }
	~AlgHandle() { if (m_h) BCryptCloseAlgorithmProvider(m_h, 0); }
	AlgHandle(const AlgHandle &) = delete;
	AlgHandle & operator=(const AlgHandle &) = delete;
	bool ok() const { return BCRYPT_SUCCESS(m_status); }
	NTSTATUS status() const { return m_status; }
	BCRYPT_ALG_HANDLE get() const { return m_h; }
private:
	BCRYPT_ALG_HANDLE m_h = nullptr;
	NTSTATUS m_status = 0;
};

class KeyHandle {
public:
	KeyHandle() = default;
	~KeyHandle() { if (m_h) BCryptDestroyKey(m_h); }
	KeyHandle(const KeyHandle &) = delete;
	KeyHandle & operator=(const KeyHandle &) = delete;
	BCRYPT_KEY_HANDLE * address() { return &m_h; }
	BCRYPT_KEY_HANDLE get() const { return m_h; }
private:
	BCRYPT_KEY_HANDLE m_h = nullptr;
};

class HashHandle {
public:
	HashHandle() = default;
	~HashHandle() { if (m_h) BCryptDestroyHash(m_h); }
	BCRYPT_HASH_HANDLE * address() { return &m_h; }
	BCRYPT_HASH_HANDLE get() const { return m_h; }
private:
	BCRYPT_HASH_HANDLE m_h = nullptr;
};

bool aes_encrypt(const std::string & plaintext, const std::string & key, const Bytes & iv,
	bool cbc, Bytes & out, std::string * error) {
	AlgHandle alg(BCRYPT_AES_ALGORITHM);
	if (!alg.ok()) return failed(error, "BCryptOpenAlgorithmProvider(AES)", alg.status());

	// 注意：BCRYPT_CHAIN_MODE_CBC / ECB 是 L"CBC" / L"ECB" 宽字符串，
	// BCryptSetProperty 要的是字节长度，不是字符数。
	LPCWSTR mode = cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
	const ULONG mode_bytes = static_cast<ULONG>((wcslen(mode) + 1) * sizeof(wchar_t));
	NTSTATUS st = BCryptSetProperty(alg.get(), BCRYPT_CHAINING_MODE,
		reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(mode)), mode_bytes, 0);
	if (!BCRYPT_SUCCESS(st)) return failed(error, "BCryptSetProperty(chaining mode)", st);

	DWORD object_len = 0, written = 0;
	st = BCryptGetProperty(alg.get(), BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &written, 0);
	if (!BCRYPT_SUCCESS(st)) return failed(error, "BCryptGetProperty(OBJECT_LENGTH)", st);

	std::vector<uint8_t> key_object(object_len);
	KeyHandle key_handle;
	st = BCryptGenerateSymmetricKey(alg.get(), key_handle.address(), key_object.data(), object_len,
		reinterpret_cast<PUCHAR>(const_cast<char *>(key.data())), static_cast<ULONG>(key.size()), 0);
	if (!BCRYPT_SUCCESS(st)) return failed(error, "BCryptGenerateSymmetricKey", st);

	Bytes padded = pkcs7_pad(plaintext);
	out.resize(padded.size());
	PUCHAR iv_ptr = nullptr;
	Bytes iv_copy;
	if (cbc) {
		iv_copy = iv;
		iv_ptr = iv_copy.data();
	}
	// BCryptEncrypt 的参数顺序是 (hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV, ...)，
	// 对称加密时 pPaddingInfo 必须为 NULL，IV 放第 5 个参数。
	st = BCryptEncrypt(key_handle.get(), padded.data(), static_cast<ULONG>(padded.size()),
		nullptr, iv_ptr, cbc ? 16u : 0u,
		out.data(), static_cast<ULONG>(out.size()), &written, 0);
	if (!BCRYPT_SUCCESS(st)) return failed(error, "BCryptEncrypt(AES)", st);
	out.resize(written);
	return true;
}

} // namespace

bool aes128_cbc_pkcs7_encrypt_b64(const std::string & plaintext, const std::string & key,
	const std::string & iv, std::string & out_b64, std::string * error) {
	if (key.size() != 16 || iv.size() != 16) {
		set_error(error, "AES-128-CBC 要求 key 与 iv 都是 16 字节");
		return false;
	}
	Bytes raw;
	if (!aes_encrypt(plaintext, key, Bytes(iv.begin(), iv.end()), true, raw, error)) return false;
	out_b64 = base64_encode(raw);
	return true;
}

bool aes128_ecb_pkcs7_encrypt_hex(const std::string & plaintext, const std::string & key,
	std::string & out_hex, std::string * error) {
	if (key.size() != 16) {
		set_error(error, "AES-128-ECB 要求 key 是 16 字节");
		return false;
	}
	Bytes raw;
	if (!aes_encrypt(plaintext, key, Bytes(), false, raw, error)) return false;
	out_hex = hex_encode(raw);
	return true;
}

bool rsa_public_raw_encrypt_hex(const Bytes & input, std::string & out_hex, std::string * error) {
	Bytes modulus, exponent;
	if (!hex_to_bytes(kRsaModulusHex, modulus) || !hex_to_bytes(kRsaExponentHex, exponent)) {
		set_error(error, "内置 RSA 常量不是合法十六进制");
		return false;
	}
	if (input.size() != modulus.size()) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), "裸 RSA 要求输入恰好 %zu 字节，实际 %zu 字节",
			modulus.size(), input.size());
		set_error(error, buf);
		return false;
	}

	AlgHandle alg(BCRYPT_RSA_ALGORITHM);
	if (!alg.ok()) return failed(error, "BCryptOpenAlgorithmProvider(RSA)", alg.status());

	BCRYPT_RSAKEY_BLOB blob{};
	blob.Magic = BCRYPT_RSAPUBLIC_MAGIC;
	blob.BitLength = static_cast<ULONG>(modulus.size() * 8);
	blob.cbPublicExp = static_cast<ULONG>(exponent.size());
	blob.cbModulus = static_cast<ULONG>(modulus.size());

	std::vector<uint8_t> key_blob(sizeof(blob) + exponent.size() + modulus.size());
	std::memcpy(key_blob.data(), &blob, sizeof(blob));
	std::memcpy(key_blob.data() + sizeof(blob), exponent.data(), exponent.size());
	std::memcpy(key_blob.data() + sizeof(blob) + exponent.size(), modulus.data(), modulus.size());

	KeyHandle key_handle;
	NTSTATUS st = BCryptImportKeyPair(alg.get(), nullptr, BCRYPT_RSAPUBLIC_BLOB, key_handle.address(),
		key_blob.data(), static_cast<ULONG>(key_blob.size()), 0);
	if (!BCRYPT_SUCCESS(st)) return failed(error, "BCryptImportKeyPair", st);

	Bytes output(modulus.size());
	ULONG written = 0;
	st = BCryptEncrypt(key_handle.get(),
		const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
		nullptr, nullptr, 0, output.data(), static_cast<ULONG>(output.size()), &written, BCRYPT_PAD_NONE);
	if (!BCRYPT_SUCCESS(st)) return failed(error, "BCryptEncrypt(RSA, PAD_NONE)", st);
	output.resize(written);
	out_hex = hex_encode(output);
	return true;
}

bool md5_hex(const std::string & data, std::string & out_hex, std::string * error) {
	AlgHandle alg(BCRYPT_MD5_ALGORITHM);
	if (!alg.ok()) return failed(error, "BCryptOpenAlgorithmProvider(MD5)", alg.status());

	DWORD object_len = 0, written = 0;
	NTSTATUS st = BCryptGetProperty(alg.get(), BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &written, 0);
	if (!BCRYPT_SUCCESS(st)) return failed(error, "BCryptGetProperty(MD5 OBJECT_LENGTH)", st);

	std::vector<uint8_t> object(object_len);
	HashHandle hash;
	st = BCryptCreateHash(alg.get(), hash.address(), object.data(), object_len, nullptr, 0, 0);
	if (!BCRYPT_SUCCESS(st)) return failed(error, "BCryptCreateHash", st);

	st = BCryptHashData(hash.get(),
		reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())), static_cast<ULONG>(data.size()), 0);
	if (!BCRYPT_SUCCESS(st)) return failed(error, "BCryptHashData", st);

	uint8_t digest[16] = {};
	st = BCryptFinishHash(hash.get(), digest, sizeof(digest), 0);
	if (!BCRYPT_SUCCESS(st)) return failed(error, "BCryptFinishHash", st);

	out_hex = hex_encode(digest, sizeof(digest));
	return true;
}

bool weapi_encrypt_with_key(const std::string & json_text, const std::string & secret_key,
	WeapiResult & out, std::string * error) {
	if (secret_key.size() != 16) {
		set_error(error, "weapi 的 secretKey 必须是 16 字符");
		return false;
	}
	std::string inner;
	if (!aes128_cbc_pkcs7_encrypt_b64(json_text, kWeapiPresetKey, kWeapiIv, inner, error)) return false;
	if (!aes128_cbc_pkcs7_encrypt_b64(inner, secret_key, kWeapiIv, out.params, error)) return false;

	// 参考实现把 secretKey 反转后当作大整数做裸 RSA，并在左侧补齐到模数长度。
	std::string reversed(secret_key.rbegin(), secret_key.rend());
	Bytes block(128, 0);
	std::memcpy(block.data() + (128 - reversed.size()), reversed.data(), reversed.size());
	if (!rsa_public_raw_encrypt_hex(block, out.enc_sec_key, error)) return false;

	out.secret_key = secret_key;
	return true;
}

bool weapi_encrypt(const std::string & json_text, WeapiResult & out, std::string * error) {
	return weapi_encrypt_with_key(json_text, random_base62(16), out, error);
}

bool eapi_encrypt(const std::string & path, const std::string & json_text,
	std::string & params_hex, std::string * error) {
	const std::string message = "nobody" + path + "use" + json_text + "md5forencrypt";
	std::string digest;
	if (!md5_hex(message, digest, error)) return false;
	const std::string data = path + "-36cd479b6b5-" + json_text + "-36cd479b6b5-" + digest;
	return aes128_ecb_pkcs7_encrypt_hex(data, kEapiKey, params_hex, error);
}

const char * rsa_modulus_hex() { return kRsaModulusHex; }
const char * rsa_exponent_hex() { return kRsaExponentHex; }
const char * weapi_preset_key() { return kWeapiPresetKey; }
const char * weapi_iv() { return kWeapiIv; }
const char * eapi_key() { return kEapiKey; }

} // namespace netease

