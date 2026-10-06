#include "core/text.h"

#include <random>

namespace netease {

namespace {
const char * kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int b64_value(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}
} // namespace

std::string base64_encode(const uint8_t * data, size_t len) {
	std::string out;
	out.reserve(((len + 2) / 3) * 4);
	for (size_t i = 0; i < len; i += 3) {
		const uint32_t b0 = data[i];
		const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
		const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
		const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
		out.push_back(kB64[(triple >> 18) & 0x3F]);
		out.push_back(kB64[(triple >> 12) & 0x3F]);
		out.push_back((i + 1 < len) ? kB64[(triple >> 6) & 0x3F] : '=');
		out.push_back((i + 2 < len) ? kB64[triple & 0x3F] : '=');
	}
	return out;
}

std::string base64_encode(const Bytes & data) {
	return base64_encode(data.empty() ? nullptr : data.data(), data.size());
}

bool base64_decode(const std::string & in, Bytes & out) {
	out.clear();
	out.reserve(in.size() / 4 * 3);
	uint32_t acc = 0;
	int bits = 0;
	for (char c : in) {
		if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
		if (c == '=') break;
		const int v = b64_value(c);
		if (v < 0) return false;
		acc = (acc << 6) | static_cast<uint32_t>(v);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
		}
	}
	return true;
}

std::string hex_encode(const uint8_t * data, size_t len) {
	static const char * kHex = "0123456789abcdef";
	std::string out;
	out.reserve(len * 2);
	for (size_t i = 0; i < len; ++i) {
		out.push_back(kHex[data[i] >> 4]);
		out.push_back(kHex[data[i] & 0x0F]);
	}
	return out;
}

std::string hex_encode(const Bytes & data) {
	return hex_encode(data.empty() ? nullptr : data.data(), data.size());
}

std::string url_encode(const std::string & s) {
	static const char * kHex = "0123456789ABCDEF";
	std::string out;
	out.reserve(s.size() * 3);
	for (unsigned char c : s) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~') {
			out.push_back(static_cast<char>(c));
		} else {
			out.push_back('%');
			out.push_back(kHex[c >> 4]);
			out.push_back(kHex[c & 0x0F]);
		}
	}
	return out;
}

std::string random_base62(size_t n) {
	static const char * kAlphabet =
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
	static std::random_device rd;
	std::uniform_int_distribution<int> dist(0, 61);
	std::string out;
	out.reserve(n);
	for (size_t i = 0; i < n; ++i) out.push_back(kAlphabet[dist(rd)]);
	return out;
}

} // namespace netease

