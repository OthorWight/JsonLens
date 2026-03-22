[CCode (cheader_filename = "arena_json.h", cname = "Arena", has_type_id = false)]
public struct Arena {
    public void* begin;
    public void* end;

    [CCode (cname = "arena_init")]
    public void init ();
    [CCode (cname = "arena_free")]
    public void free ();
    [CCode (cname = "arena_reset")]
    public void reset ();
}

[CCode (cheader_filename = "arena_json.h", cname = "JsonError", has_type_id = false)]
public struct JsonError {
    public char msg[128];
    public int line;
    public int col;
    public size_t offset;
}

[CCode (cheader_filename = "arena_json.h", cname = "JsonType", cprefix = "JSON_", has_type_id = false)]
public enum JsonType {
    NULL, BOOL, NUMBER, STRING, ARRAY, OBJECT
}

[CCode (cheader_filename = "arena_json.h", cname = "JsonNode", has_type_id = false)]
public struct JsonNode {
    public weak string key;
    public JsonValue value;
}

[CCode (cheader_filename = "arena_json.h", cname = "JsonValue", has_type_id = false)]
public struct JsonValue {
    public JsonType type;
    public size_t offset;
    [CCode (cname = "as.boolean")]
    public bool boolean;
    [CCode (cname = "as.number")]
    public double number;
    [CCode (cname = "as.string")]
    public weak string string_val;
    [CCode (cname = "as.list.count")]
    public size_t list_count;
    [CCode (cname = "as.list.items", array_length = false)]
    public JsonNode* list_items;
}

[CCode (cheader_filename = "arena_json.h", cname = "json_parse")]
public unowned JsonValue* json_parse (ref Arena main, ref Arena scratch, string input, size_t len, int flags, out JsonError err);
[CCode (cheader_filename = "arena_json.h", cname = "json_to_string")]
public unowned string? json_to_string (ref Arena a, JsonValue* v, bool pretty);
[CCode (cheader_filename = "arena_json.h", cname = "json_create_null")]
public JsonValue* json_create_null (ref Arena a);
[CCode (cheader_filename = "arena_json.h", cname = "json_create_bool")]
public JsonValue* json_create_bool (ref Arena a, bool b);
[CCode (cheader_filename = "arena_json.h", cname = "json_create_number")]
public JsonValue* json_create_number (ref Arena a, double num);
[CCode (cheader_filename = "arena_json.h", cname = "json_create_string")]
public JsonValue* json_create_string (ref Arena a, string str);
[CCode (cheader_filename = "arena_json.h", cname = "json_create_array")]
public JsonValue* json_create_array (ref Arena a);
[CCode (cheader_filename = "arena_json.h", cname = "json_create_object")]
public JsonValue* json_create_object (ref Arena a);
[CCode (cheader_filename = "arena_json.h", cname = "json_add")]
public void json_add (ref Arena a, JsonValue* obj, string key, JsonValue* val);
[CCode (cheader_filename = "arena_json.h", cname = "json_append")]
public void json_append (ref Arena a, JsonValue* arr, JsonValue* val);
[CCode (cheader_filename = "arena_json.h", cname = "json_get_number")]
public double json_get_number (JsonValue* obj, string key, double fallback);
[CCode (cheader_filename = "arena_json.h", cname = "json_get_string")]
public unowned string? json_get_string (JsonValue* obj, string key, string fallback);
[CCode (cheader_filename = "arena_json.h", cname = "json_get_bool")]
public bool json_get_bool (JsonValue* obj, string key, bool fallback);
[CCode (cheader_filename = "arena_json.h", cname = "json_get")]
public JsonValue* json_get (JsonValue* obj, string key);
[CCode (cheader_filename = "arena_json.h", cname = "json_replace_in_object")]
public void json_replace_in_object (ref Arena a, JsonValue* obj, string key, JsonValue* new_val);