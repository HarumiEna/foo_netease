#pragma once
// foo_netease —— 极简 JSON。
//
// 为什么不用第三方库：SDK 本身不带 JSON 解析，而组件希望保持零外部依赖。
// 我们需要的只是「解析响应 + 取值 + 组请求体」，下面这些就够了。
//
// 设计取舍：
//  · 数字保留原始文本（raw），避免大的歌曲 id 走 double 丢精度；
//  · 对象用有序数组保存，便于调试输出时字段顺序稳定。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace netease {
namespace json {

class Value {
public:
	enum class Type { Null, Bool, Number, String, Array, Object };

	Type type = Type::Null;
	bool boolean = false;
	std::string text;                        // String 的值 / Number 的原始文本
	std::vector<Value> items;                // Array
	std::vector<std::pair<std::string, Value>> members; // Object

	Value() = default;

	bool is_null() const { return type == Type::Null; }
	bool is_bool() const { return type == Type::Bool; }
	bool is_number() const { return type == Type::Number; }
	bool is_string() const { return type == Type::String; }
	bool is_array() const { return type == Type::Array; }
	bool is_object() const { return type == Type::Object; }

	// 对象取值；不存在返回 nullptr。
	const Value * find(const char * key) const;
	const Value * find(const std::string & key) const;

	// 便捷取值（类型不符时返回默认值）。
	int64_t as_int64(int64_t def = 0) const;
	double as_double(double def = 0) const;
	std::string as_string(const std::string & def = std::string()) const;
	bool as_bool(bool def = false) const;

	size_t size() const { return items.empty() ? members.size() : items.size(); }
	const Value * at(size_t index) const;

	// 调试输出（紧凑，不转义非 ASCII）。
	std::string dump() const;

	static bool parse(const std::string & text, Value & out, std::string * error);
};

std::string escape(const std::string & s);

} // namespace json
} // namespace netease

