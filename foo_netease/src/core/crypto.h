#pragma once
// foo_netease —— 网易云 weapi / eapi 请求体加密。
//
// 全部使用 Windows CNG（BCrypt）实现，不引入第三方加密库：
//  · AES-128-CBC / AES-128-ECB：weapi 与 eapi 用；
//  · RSA 1024 裸加密（无填充，BCRYPT_PAD_NONE）：weapi 的 encSecKey 用；
//  · MD5：eapi 的摘要用。
//
// 常量来源：见 crypto.cpp 顶部注释。
// 本层的正确性用固定输入与 Node 参考实现的结果逐字节比对过。

#include <cstdint>
#include <string>
#include <vector>

#include "core/text.h"

namespace netease {

struct WeapiResult {
	std::string params;       // base64
	std::string enc_sec_key;  // hex，256 字符
	std::string secret_key;   // 调试用；生产路径不要记录进日志
};

// 随机生成 secretKey 的标准 weapi 加密。
bool weapi_encrypt(const std::string & json_text, WeapiResult & out, std::string * error);

// 指定 secretKey 的版本，用于与参考实现做确定性比对。
bool weapi_encrypt_with_key(const std::string & json_text, const std::string & secret_key,
	WeapiResult & out, std::string * error);

// eapi：返回 params（hex）。
bool eapi_encrypt(const std::string & path, const std::string & json_text,
	std::string & params_hex, std::string * error);

bool aes128_cbc_pkcs7_encrypt_b64(const std::string & plaintext, const std::string & key,
	const std::string & iv, std::string & out_b64, std::string * error);

bool aes128_ecb_pkcs7_encrypt_hex(const std::string & plaintext, const std::string & key,
	std::string & out_hex, std::string * error);

// 输入必须是 128 字节（1024 位模数的长度）。
bool rsa_public_raw_encrypt_hex(const Bytes & input, std::string & out_hex, std::string * error);

bool md5_hex(const std::string & data, std::string & out_hex, std::string * error);

// 常量，供自检输出使用。
const char * rsa_modulus_hex();
const char * rsa_exponent_hex();
const char * weapi_preset_key();
const char * weapi_iv();
const char * eapi_key();

} // namespace netease

