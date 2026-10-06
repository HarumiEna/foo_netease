#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace netease {
namespace json {

const Value * Value::find(const std::string & key) const {
	if (type != Type::Object) return nullptr;
	for (const auto & kv : members) {
		if (kv.first == key) return &kv.second;
	}
	return nullptr;
}
const Value * Value::find(const char * key) const {
	return find(std::string(key));
}

int64_t Value::as_int64(int64_t def) const {
	if (type == Type::Number) {
		errno = 0;
		const long long v = std::strtoll(text.c_str(), nullptr, 10);
		return errno == 0 ? static_cast<int64_t>(v) : def;
	}
	if (type == Type::Bool) return boolean ? 1 : 0;
	return def;
}

double Value::as_double(double def) const {
	if (type == Type::Number) return std::strtod(text.c_str(), nullptr);
	return def;
}

std::string Value::as_string(const std::string & def) const {
	if (type == Type::String) return text;
	if (type == Type::Number) return text;
	return def;
}

bool Value::as_bool(bool def) const {
	if (type == Type::Bool) return boolean;
	if (type == Type::Number) return as_int64() != 0;
	return def;
}

const Value * Value::at(size_t index) const {
	if (type != Type::Array || index >= items.size()) return nullptr;
	return &items[index];
}

namespace {

class Parser {
public:
	explicit Parser(const std::string & s) : m_s(s) {}

	bool parse(Value & out) {
		skip_ws();
		if (!parse_value(out)) return false;
		skip_ws();
		if (m_pos != m_s.size()) return fail("多余内容");
		return true;
	}

	const std::string & error() const { return m_error; }

private:
	const std::string & m_s;
	size_t m_pos = 0;
	std::string m_error;

	bool fail(const char * what) {
		if (m_error.empty()) {
			char buf[128];
			std::snprintf(buf, sizeof(buf), "JSON 解析失败（位置 %zu）：%s", m_pos, what);
			m_error = buf;
		}
		return false;
	}

	void skip_ws() {
		while (m_pos < m_s.size()) {
			const char c = m_s[m_pos];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++m_pos; continue; }
			break;
		}
	}

	bool parse_value(Value & out) {
		skip_ws();
		if (m_pos >= m_s.size()) return fail("期望一个值");
		switch (m_s[m_pos]) {
		case '{': return parse_object(out);
		case '[': return parse_array(out);
		case '"': { out.type = Value::Type::String; return parse_string(out.text); }
		case 't': return literal("true", out, true);
		case 'f': return literal("false", out, false);
		case 'n': {
			if (m_s.compare(m_pos, 4, "null") != 0) return fail("非法字面量");
			m_pos += 4; out.type = Value::Type::Null; return true;
		}
		default: return parse_number(out);
		}
	}

	bool literal(const char * word, Value & out, bool value) {
		const size_t n = std::string(word).size();
		if (m_s.compare(m_pos, n, word) != 0) return fail("非法字面量");
		m_pos += n;
		out.type = Value::Type::Bool;
		out.boolean = value;
		return true;
	}

	bool parse_object(Value & out) {
		out.type = Value::Type::Object;
		++m_pos; // {
		skip_ws();
		if (m_pos < m_s.size() && m_s[m_pos] == '}') { ++m_pos; return true; }
		for (;;) {
			skip_ws();
			std::string key;
			if (m_pos >= m_s.size() || m_s[m_pos] != '"') return fail("对象键必须是字符串");
			if (!parse_string(key)) return false;
			skip_ws();
			if (m_pos >= m_s.size() || m_s[m_pos] != ':') return fail("缺少冒号");
			++m_pos;
			Value v;
			if (!parse_value(v)) return false;
			out.members.emplace_back(std::move(key), std::move(v));
			skip_ws();
			if (m_pos >= m_s.size()) return fail("对象未闭合");
			if (m_s[m_pos] == ',') { ++m_pos; continue; }
			if (m_s[m_pos] == '}') { ++m_pos; return true; }
			return fail("对象中出现意外字符");
		}
	}

	bool parse_array(Value & out) {
		out.type = Value::Type::Array;
		++m_pos; // [
		skip_ws();
		if (m_pos < m_s.size() && m_s[m_pos] == ']') { ++m_pos; return true; }
		for (;;) {
			Value v;
			if (!parse_value(v)) return false;
			out.items.push_back(std::move(v));
			skip_ws();
			if (m_pos >= m_s.size()) return fail("数组未闭合");
			if (m_s[m_pos] == ',') { ++m_pos; continue; }
			if (m_s[m_pos] == ']') { ++m_pos; return true; }
			return fail("数组中出现意外字符");
		}
	}

	static void append_utf8(std::string & out, uint32_t cp) {
		if (cp <= 0x7F) {
			out.push_back(static_cast<char>(cp));
		} else if (cp <= 0x7FF) {
			out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else if (cp <= 0xFFFF) {
			out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}

	bool parse_hex4(uint32_t & out) {
		if (m_pos + 4 > m_s.size()) return fail("\\\\u 转义不完整");
		out = 0;
		for (int i = 0; i < 4; ++i) {
			const char c = m_s[m_pos++];
			uint32_t d;
			if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A' + 10);
			else return fail("\\\\u 转义含非法字符");
			out = (out << 4) | d;
		}
		return true;
	}

	bool parse_string(std::string & out) {
		out.clear();
		++m_pos; // 开引号
		while (m_pos < m_s.size()) {
			const unsigned char c = static_cast<unsigned char>(m_s[m_pos++]);
			if (c == '"') return true;
			if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
			if (m_pos >= m_s.size()) return fail("反斜杠后没有内容");
			const char e = m_s[m_pos++];
			switch (e) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				uint32_t cp = 0;
				if (!parse_hex4(cp)) return false;
				if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 1 < m_s.size() &&
					m_s[m_pos] == '\\' && m_s[m_pos + 1] == 'u') {
					m_pos += 2;
					uint32_t low = 0;
					if (!parse_hex4(low)) return false;
					if (low >= 0xDC00 && low <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					}
				}
				append_utf8(out, cp);
				break;
			}
			default: return fail("未知转义序列");
			}
		}
		return fail("字符串未闭合");
	}

	bool parse_number(Value & out) {
		const size_t start = m_pos;
		if (m_pos < m_s.size() && (m_s[m_pos] == '-' || m_s[m_pos] == '+')) ++m_pos;
		bool any = false;
		while (m_pos < m_s.size() &&
			((m_s[m_pos] >= '0' && m_s[m_pos] <= '9') || m_s[m_pos] == '.' ||
				m_s[m_pos] == 'e' || m_s[m_pos] == 'E' || m_s[m_pos] == '-' || m_s[m_pos] == '+')) {
			any = true;
			++m_pos;
		}
		if (!any) { m_pos = start; return fail("非法值"); }
		out.type = Value::Type::Number;
		out.text = m_s.substr(start, m_pos - start);
		return true;
	}
};

} // namespace

bool Value::parse(const std::string & text, Value & out, std::string * error) {
	Parser parser(text);
	if (!parser.parse(out)) {
		if (error) *error = parser.error();
		return false;
	}
	return true;
}

std::string escape(const std::string & s) {
	std::string out;
	out.reserve(s.size() + 8);
	for (char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
				out += buf;
			} else {
				out.push_back(c);
			}
		}
	}
	return out;
}

namespace {
void dump_into(const Value & v, std::string & out) {
	switch (v.type) {
	case Value::Type::Null: out += "null"; break;
	case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
	case Value::Type::Number: out += v.text.empty() ? "0" : v.text; break;
	case Value::Type::String: out += "\"" + escape(v.text) + "\""; break;
	case Value::Type::Array: {
		out.push_back('[');
		for (size_t i = 0; i < v.items.size(); ++i) {
			if (i) out.push_back(',');
			dump_into(v.items[i], out);
		}
		out.push_back(']');
		break;
	}
	case Value::Type::Object: {
		out.push_back('{');
		for (size_t i = 0; i < v.members.size(); ++i) {
			if (i) out.push_back(',');
			out += "\"" + escape(v.members[i].first) + "\":";
			dump_into(v.members[i].second, out);
		}
		out.push_back('}');
		break;
	}
	}
}
} // namespace

std::string Value::dump() const {
	std::string out;
	dump_into(*this, out);
	return out;
}

} // namespace json
} // namespace netease

