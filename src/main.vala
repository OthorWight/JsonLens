// src/main.vala

[CCode (cname = "gettext", cheader_filename = "libintl.h")]
public extern unowned string _ (string msgid);

[CCode (cname = "g_ascii_strtod", cheader_filename = "glib.h")]
public extern double parse_ascii_double (string nptr, out unowned string endptr);

[CCode (cname = "gtk_style_context_add_provider_for_display", cheader_filename = "gtk/gtk.h")]
public extern void add_provider_for_display (Gdk.Display display, Gtk.StyleProvider provider, uint priority);

public class ParseResult {
    public string? formatted_json;
    public JsonError err;
    public bool success;
}

public class FileState {
    public string uri;
    public string view_name = "text";
    public int cursor_offset = 0;
    public string[] column_path = new string[0];
}

public class JsonColumnView : Gtk.Box {
    private Arena arena;
    private Arena scratch;
    private Gtk.Box columns_box;
    private Gtk.ScrolledWindow scroll;
    public string last_loaded_text = "";
    public size_t selected_offset = 0;
    public string[] pending_restore_path = new string[0];
    public bool user_clicked_column = false;
    
    public signal void jump_to_text ();
    
    public JsonColumnView () {
        GLib.Object (orientation: Gtk.Orientation.HORIZONTAL, spacing: 0);
        arena = {};
        arena.init ();
        scratch = {};
        scratch.init ();
        
        scroll = new Gtk.ScrolledWindow ();
        scroll.set_policy (Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.NEVER);
        scroll.hexpand = true;
        scroll.vexpand = true;
        
        columns_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 6);
        columns_box.margin_start = columns_box.margin_end = 6;
        columns_box.margin_top = columns_box.margin_bottom = 6;
        
        scroll.set_child (columns_box);
        
        append (scroll);
    }
    
    ~JsonColumnView () {
        scratch.free ();
        arena.free ();
    }
    
    public string[] get_current_path () {
        string[] path = new string[0];
        var child = columns_box.get_first_child ();
        while (child != null) {
            var frame = child as Gtk.Frame;
            if (frame != null) {
                var sw = frame.get_child () as Gtk.ScrolledWindow;
                if (sw != null) {
                    var sw_child = sw.get_child ();
                    var viewport = sw_child as Gtk.Viewport;
                    Gtk.ListBox? list_box = null;
                    if (viewport != null) {
                        list_box = viewport.get_child () as Gtk.ListBox;
                    } else {
                        list_box = sw_child as Gtk.ListBox;
                    }
                    if (list_box != null) {
                        var row = list_box.get_selected_row ();
                        if (row != null) {
                            string key = row.get_data<string> ("json_key");
                            if (key != null) path += key;
                        }
                    }
                }
            }
            child = child.get_next_sibling ();
        }
        
        if (path.length == 0 && pending_restore_path.length > 0) {
            return pending_restore_path;
        }
        
        return path;
    }

    public void load_json (string json_text) {
        if (last_loaded_text == json_text) return;
        
        string[] path_to_restore = get_current_path ();
        last_loaded_text = json_text;
        
        scratch.free ();
        arena.free ();
        arena.init ();
        scratch.init ();
        
        while (columns_box.get_first_child () != null) {
            columns_box.remove (columns_box.get_first_child ());
        }
        selected_offset = 0;
        
        JsonError err;
        JsonValue* root = json_parse (ref arena, ref scratch, json_text, json_text.length, 1, out err);
        if (root != null) {
            pending_restore_path = path_to_restore;
            push_column (root, "Root", null, 0);
            pending_restore_path = new string[0];
        } else {
            pending_restore_path = path_to_restore;
            var error_label = new Gtk.Label ("Invalid JSON: " + (string) err.msg);
            error_label.add_css_class ("error");
            columns_box.append (error_label);
        }
    }

    private void push_column (JsonValue* val, string title, Gtk.Widget? after_column = null, int path_index = 0) {
        bool found = (after_column == null);
        var child = columns_box.get_first_child ();
        while (child != null) {
            var next = child.get_next_sibling ();
            if (found && child != after_column) {
                columns_box.remove (child);
            }
            if (child == after_column) found = true;
            child = next;
        }
        
        var list_box = new Gtk.ListBox ();
        list_box.width_request = 250;
        list_box.selection_mode = Gtk.SelectionMode.SINGLE;
        list_box.add_css_class ("rich-list");
        
        var click_controller = new Gtk.GestureClick ();
        click_controller.button = Gdk.BUTTON_PRIMARY;
        click_controller.pressed.connect ((n_press, x, y) => {
            if (n_press == 2) {
                var row = list_box.get_row_at_y ((int) y);
                if (row != null) {
                    JsonValue* child_val = (JsonValue*) row.get_data<void*> ("json_val");
                    if (child_val != null) {
                        this.selected_offset = child_val->offset;
                        this.user_clicked_column = true;
                        this.jump_to_text ();
                    }
                }
            }
        });
        list_box.add_controller (click_controller);
        
        var col_scroll = new Gtk.ScrolledWindow ();
        col_scroll.set_policy (Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC);
        col_scroll.set_child (list_box);
        
        var frame = new Gtk.Frame (title);
        frame.set_child (col_scroll);
        frame.vexpand = true;
        frame.set_data<void*> ("json_val", val);
        
        Gtk.ListBoxRow? row_to_select = null;
        
        if (val->type == JsonType.OBJECT) {
            for (size_t i = 0; i < val->list_count; i++) {
                JsonNode* node = val->list_items + i;
                var row = new Gtk.ListBoxRow ();
                var box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 6);
                var lbl = new Gtk.Label (node->key);
                lbl.xalign = 0;
                lbl.hexpand = true;
                lbl.ellipsize = Pango.EllipsizeMode.END;
                box.append (lbl);
                
                if (node->value.type == JsonType.OBJECT || node->value.type == JsonType.ARRAY) {
                    var arrow = new Gtk.Image.from_icon_name ("go-next-symbolic");
                    arrow.add_css_class ("dim-label");
                    box.append (arrow);
                } else {
                    var val_lbl = new Gtk.Label (get_preview_text (&node->value));
                    val_lbl.add_css_class ("dim-label");
                    val_lbl.ellipsize = Pango.EllipsizeMode.END;
                    val_lbl.max_width_chars = 15;
                    box.append (val_lbl);
                }
                
                box.margin_start = box.margin_end = 6;
                box.margin_top = box.margin_bottom = 6;
                row.set_child (box);
                
                row.set_data<void*> ("json_val", &node->value);
                row.set_data<string> ("json_key", node->key);
                list_box.append (row);
                
                if (pending_restore_path.length > path_index && node->key == pending_restore_path[path_index]) {
                    row_to_select = row;
                }
            }
        } else if (val->type == JsonType.ARRAY) {
            for (size_t i = 0; i < val->list_count; i++) {
                JsonNode* node = val->list_items + i;
                var row = new Gtk.ListBoxRow ();
                var box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 6);
                var lbl = new Gtk.Label ("[%zu]".printf(i));
                lbl.xalign = 0;
                lbl.hexpand = true;
                box.append (lbl);

                if (node->value.type == JsonType.OBJECT || node->value.type == JsonType.ARRAY) {
                    var arrow = new Gtk.Image.from_icon_name ("go-next-symbolic");
                    arrow.add_css_class ("dim-label");
                    box.append (arrow);
                } else {
                    var val_lbl = new Gtk.Label (get_preview_text (&node->value));
                    val_lbl.add_css_class ("dim-label");
                    val_lbl.ellipsize = Pango.EllipsizeMode.END;
                    val_lbl.max_width_chars = 15;
                    box.append (val_lbl);
                }
                
                box.margin_start = box.margin_end = 6;
                box.margin_top = box.margin_bottom = 6;
                row.set_child (box);
                
                row.set_data<void*> ("json_val", &node->value);
                row.set_data<string> ("json_key", "[%zu]".printf(i));
                list_box.append (row);
                
                string arr_key = "[%zu]".printf(i);
                if (pending_restore_path.length > path_index && arr_key == pending_restore_path[path_index]) {
                    row_to_select = row;
                }
            }
        } else {
            var lbl = new Gtk.Label (get_preview_text (val));
            lbl.wrap = true;
            lbl.margin_start = 6; lbl.margin_end = 6;
            lbl.margin_top = lbl.margin_bottom = 6;
            var row = new Gtk.ListBoxRow ();
            row.set_child (lbl);
            row.selectable = false;
            row.activatable = false;
            list_box.append (row);
        }
        
        list_box.row_selected.connect ((row) => {
            if (row != null) {
                JsonValue* child_val = (JsonValue*) row.get_data<void*> ("json_val");
                string child_key = row.get_data<string> ("json_key");
                if (child_val != null) {
                    var next_col = frame.get_next_sibling ();
                    if (next_col != null) {
                        JsonValue* next_val = (JsonValue*) next_col.get_data<void*> ("json_val");
                        if (next_val == child_val) {
                            return;
                        }
                    }
                    
                    if (pending_restore_path.length == 0) {
                        user_clicked_column = true;
                    }
                    this.selected_offset = child_val->offset;
                    push_column (child_val, child_key, frame, path_index + 1);
                    GLib.Idle.add (() => {
                        var adj = scroll.get_hadjustment ();
                        adj.set_value (adj.get_upper () - adj.get_page_size ());
                        return GLib.Source.REMOVE;
                    });
                }
            }
        });
        
        columns_box.append (frame);
        
        if (row_to_select != null) {
            list_box.select_row (row_to_select);
            
            var next_col = frame.get_next_sibling ();
            if (next_col == null) {
                JsonValue* child_val = (JsonValue*) row_to_select.get_data<void*> ("json_val");
                string child_key = row_to_select.get_data<string> ("json_key");
                if (child_val != null) {
                    this.selected_offset = child_val->offset;
                    push_column (child_val, child_key, frame, path_index + 1);
                }
            }
        }
    }

    private string get_preview_text (JsonValue* val) {
        if (val->type == JsonType.STRING) return "\"%s\"".printf(val->string_val);
        if (val->type == JsonType.NUMBER) {
            double n = val->number;
            if (n == (int64) n) {
                return ((int64) n).to_string ();
            } else {
                return "%g".printf (n);
            }
        }
        if (val->type == JsonType.BOOL) return val->boolean ? "true" : "false";
        if (val->type == JsonType.NULL) return "null";
        if (val->type == JsonType.OBJECT) return "{...}";
        if (val->type == JsonType.ARRAY) return "[...]";
        return "";
    }
}

public class JsonGraphView : Gtk.Box {
    private Arena arena;
    private Arena scratch;
    private Gtk.ScrolledWindow scroll;
    private Gtk.DrawingArea drawing_area;
    public string last_loaded_text = "";
    public size_t selected_offset = 0;
    public bool user_clicked_node = false;
    
    public signal void jump_to_text ();

    private class DrawNode {
        public string label = "";
        public DrawNode[] children = new DrawNode[0];
        public double x = 0;
        public double y = 0;
        public double box_width = 0;
        public double drawn_width = 0;
        public size_t offset = 0;
        public bool is_fake = false;
    }

    private DrawNode? root_node = null;
    private double total_width = 0;
    private double total_height = 0;

    public JsonGraphView () {
        GLib.Object (orientation: Gtk.Orientation.HORIZONTAL, spacing: 0);
        arena = {}; arena.init ();
        scratch = {}; scratch.init ();

        scroll = new Gtk.ScrolledWindow ();
        scroll.set_policy (Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC);
        scroll.hexpand = true;
        scroll.vexpand = true;

        drawing_area = new Gtk.DrawingArea ();
        drawing_area.focusable = true;
        
        var click_controller = new Gtk.GestureClick ();
        click_controller.button = Gdk.BUTTON_PRIMARY;
        click_controller.pressed.connect ((n_press, x, y) => {
            drawing_area.grab_focus ();
            if (n_press == 2 && root_node != null) {
                var hit = hit_test (root_node, x, y);
                if (hit != null && !hit.is_fake) {
                    this.selected_offset = hit.offset;
                    this.user_clicked_node = true;
                    this.jump_to_text ();
                }
            }
        });
        drawing_area.add_controller (click_controller);
        
        drawing_area.set_draw_func ((area, cr, width, height) => {
            cr.set_source_rgb (0.15, 0.15, 0.15);
            cr.paint ();
            
            if (root_node == null) {
                cr.set_source_rgb (0.8, 0.8, 0.8);
                cr.select_font_face ("Sans", Cairo.FontSlant.NORMAL, Cairo.FontWeight.BOLD);
                cr.set_font_size (24.0);
                
                string title = _("Graph View (No Data or Invalid JSON)");
                Cairo.TextExtents extents;
                cr.text_extents (title, out extents);
                cr.move_to (width / 2.0 - extents.width / 2.0, height / 2.0);
                cr.show_text (title);
                return;
            }

            cr.select_font_face ("Sans", Cairo.FontSlant.NORMAL, Cairo.FontWeight.NORMAL);
            cr.set_font_size (12.0);
            
            draw_edges (cr, root_node);
            draw_nodes (cr, root_node);
        });
        scroll.set_child (drawing_area);
        append (scroll);
    }

    private DrawNode? hit_test (DrawNode node, double px, double py) {
        double box_height = 24.0;
        double rx = node.x;
        double ry = node.y - box_height / 2.0;
        
        if (px >= rx && px <= rx + node.drawn_width && py >= ry && py <= ry + box_height) {
            return node;
        }
        
        foreach (var child in node.children) {
            var hit = hit_test (child, px, py);
            if (hit != null) return hit;
        }
        return null;
    }

    ~JsonGraphView () {
        scratch.free ();
        arena.free ();
    }

    public void load_json (string json_text) {
        if (last_loaded_text == json_text) return;
        last_loaded_text = json_text;

        scratch.free ();
        arena.free ();
        arena.init ();
        scratch.init ();
        root_node = null;

        JsonError err;
        JsonValue* root = json_parse (ref arena, ref scratch, json_text, json_text.length, 1, out err);
        
        if (root != null) {
            root_node = build_graph (root, "Root");
            current_y = 40.0;
            max_x = 0.0;
            if (root_node != null) {
                layout_node (root_node, 0);
                total_width = max_x + 400.0;
                total_height = current_y + 40.0;
            } else {
                total_width = 0;
                total_height = 0;
            }
        }
        
        drawing_area.set_size_request ((int)total_width, (int)total_height);
        drawing_area.queue_draw ();
    }

    private DrawNode? build_graph (JsonValue* val, string key) {
        if (val == null) return null;
        var node = new DrawNode ();
        node.offset = val->offset;
        
        string preview = "";
        if (val->type == JsonType.STRING) {
            string s = val->string_val;
            if (s.length > 20) s = s.substring(0, 17) + "...";
            preview = "\"%s\"".printf(s);
        }
        else if (val->type == JsonType.NUMBER) {
            double n = val->number;
            if (n == (int64) n) preview = ((int64) n).to_string ();
            else preview = "%g".printf (n);
        }
        else if (val->type == JsonType.BOOL) preview = val->boolean ? "true" : "false";
        else if (val->type == JsonType.NULL) preview = "null";
        else if (val->type == JsonType.OBJECT) preview = "{}";
        else if (val->type == JsonType.ARRAY) preview = "[]";

        node.label = key != "" ? "%s: %s".printf(key, preview) : preview;
        node.box_width = node.label.length * 7.0 + 16.0;

        if (val->type == JsonType.OBJECT) {
            size_t count = val->list_count > 50 ? 50 : val->list_count;
            for (size_t i = 0; i < count; i++) {
                JsonNode* child = val->list_items + i;
                var child_node = build_graph (&child->value, child->key);
                if (child_node != null) node.children += child_node;
            }
            if (val->list_count > 50) {
                var more = new DrawNode();
                more.label = "... %zu more".printf(val->list_count - 50);
                more.box_width = more.label.length * 7.0 + 16.0;
                more.is_fake = true;
                node.children += more;
            }
        } else if (val->type == JsonType.ARRAY) {
            size_t count = val->list_count > 50 ? 50 : val->list_count;
            for (size_t i = 0; i < count; i++) {
                JsonNode* child = val->list_items + i;
                var child_node = build_graph (&child->value, "[%zu]".printf(i));
                if (child_node != null) node.children += child_node;
            }
            if (val->list_count > 50) {
                var more = new DrawNode();
                more.label = "... %zu more".printf(val->list_count - 50);
                more.box_width = more.label.length * 7.0 + 16.0;
                more.is_fake = true;
                node.children += more;
            }
        }
        return node;
    }

    private double current_y = 40.0;
    private double max_x = 0;

    private void layout_node (DrawNode node, double depth) {
        node.x = 40.0 + depth * 220.0;
        if (node.x > max_x) max_x = node.x;

        if (node.children.length == 0) {
            node.y = current_y;
            current_y += 34.0;
        } else {
            double start_y = current_y;
            foreach (var child in node.children) {
                layout_node (child, depth + 1.0);
            }
            node.y = (start_y + (current_y - 34.0)) / 2.0;
        }
    }

    private void draw_edges (Cairo.Context cr, DrawNode node) {
        cr.set_source_rgb (0.4, 0.4, 0.4);
        cr.set_line_width (1.5);
        foreach (var child in node.children) {
            cr.move_to (node.x + node.box_width, node.y);
            cr.curve_to (node.x + node.box_width + 40.0, node.y, 
                         child.x - 40.0, child.y, 
                         child.x, child.y);
            cr.stroke ();
            draw_edges (cr, child);
        }
    }

    private void draw_nodes (Cairo.Context cr, DrawNode node) {
        Cairo.TextExtents extents;
        cr.text_extents (node.label, out extents);
        
        double drawn_width = extents.width + 16.0;
        node.drawn_width = drawn_width;
        double box_height = 24.0;
        double rx = node.x;
        double ry = node.y - box_height / 2.0;

        cr.set_source_rgba (0.2, 0.2, 0.25, 0.9);
        cr.rectangle (rx, ry, drawn_width, box_height);
        cr.fill_preserve ();
        
        cr.set_source_rgb (0.5, 0.5, 0.6);
        cr.set_line_width (1.0);
        cr.stroke ();

        cr.set_source_rgb (0.9, 0.9, 0.9);
        cr.move_to (rx + 8.0, node.y + extents.height / 2.0 - 2.0);
        cr.show_text (node.label);

        foreach (var child in node.children) {
            draw_nodes (cr, child);
        }
    }
}

public class LineNumberGutterRenderer : GtkSource.GutterRendererText {
    private GtkSource.Buffer buf;
    private int current_digits = 3;

    public LineNumberGutterRenderer (GtkSource.Buffer buf) {
        this.buf = buf;
        this.alignment_mode = GtkSource.GutterRendererAlignmentMode.FIRST;
        this.xalign = 1.0f;
        this.xpad = 4;
        this.add_css_class ("dim-label");

        buf.changed.connect (() => {
            int lines = buf.get_line_count ();
            int digits = 1;
            while (lines >= 10) { digits++; lines /= 10; }
            if (digits < 3) digits = 3;
            if (digits != current_digits) {
                current_digits = digits;
                this.queue_resize ();
            }
        });
    }

    public override void measure (Gtk.Orientation orientation, int for_size, out int minimum, out int natural, out int minimum_baseline, out int natural_baseline) {
        if (orientation == Gtk.Orientation.HORIZONTAL) {
            string max_str = "";
            for (int i = 0; i < current_digits; i++) max_str += "9";
            int w, h;
            base.measure (max_str, out w, out h);
            minimum = natural = w;
        } else {
            int w, h;
            base.measure ("9", out w, out h);
            minimum = natural = h;
        }
        minimum_baseline = natural_baseline = -1;
    }

    public override void query_data (GLib.Object lines, uint line) {
        Gtk.TextIter iter;
        buf.get_iter_at_line (out iter, (int) line);
        
        var fold_tag = buf.tag_table.lookup ("folded");
        if (fold_tag != null && iter.has_tag (fold_tag)) {
            this.text = "";
            return;
        }
        
        this.text = "%u".printf (line + 1);
    }
}

public class FoldGutterRenderer : GtkSource.GutterRendererText {
    private GtkSource.Buffer buf;
    private EditorTab tab;

    public FoldGutterRenderer (EditorTab tab) {
        this.tab = tab;
        this.buf = tab.text_buffer;
        this.alignment_mode = GtkSource.GutterRendererAlignmentMode.FIRST;
        this.xalign = 0.5f;
        this.xpad = 4;
        this.add_css_class ("dim-label");
        
        this.query_activatable.connect ((iter, area) => {
            if (iter.get_chars_in_line () > 2000) return false;
            Gtk.TextIter scan = iter;
            int max_scan = 256;
            while (!scan.ends_line () && !scan.is_end () && max_scan > 0) {
                unichar c = scan.get_char ();
                if ((c == '{' || c == '[') && !buf.iter_has_context_class (scan, "string")) {
                    Gtk.TextIter next = scan;
                    next.forward_char ();
                    var fold_tag = buf.tag_table.lookup ("folded");
                    if (fold_tag != null && next.has_tag (fold_tag)) return true;
                    return is_foldable (scan);
                }
                scan.forward_char ();
                max_scan--;
            }
            return false;
        });

        this.activate.connect ((iter, area, button, state, n_presses) => {
            if (iter.get_chars_in_line () > 2000) return;
            Gtk.TextIter scan = iter;
            int max_scan = 256;
            while (!scan.ends_line () && !scan.is_end () && max_scan > 0) {
                unichar c = scan.get_char ();
                if ((c == '{' || c == '[') && !buf.iter_has_context_class (scan, "string")) {
                    tab.toggle_fold_at (scan);
                    return;
                }
                scan.forward_char ();
                max_scan--;
            }
        });
    }

    private bool is_foldable (Gtk.TextIter open_bracket) {
        Gtk.TextIter forward_iter = open_bracket;
        unichar open_char = forward_iter.get_char ();
        unichar target_close = (open_char == '{') ? '}' : ']';
        int nesting = 0;
        int max_scan = 50000;
        int start_line = open_bracket.get_line ();
        
        while (!forward_iter.is_end () && max_scan > 0) {
            max_scan--;
            unichar c = forward_iter.get_char ();
            if (!buf.iter_has_context_class (forward_iter, "string") && !buf.iter_has_context_class (forward_iter, "comment")) {
                if (c == open_char) {
                    nesting++;
                } else if (c == target_close) {
                    nesting--;
                    if (nesting == 0) return forward_iter.get_line () > start_line;
                }
            }
            forward_iter.forward_char ();
        }
        return false;
    }

    public override void measure (Gtk.Orientation orientation, int for_size, out int minimum, out int natural, out int minimum_baseline, out int natural_baseline) {
        if (orientation == Gtk.Orientation.HORIZONTAL) {
            int w, h;
            base.measure ("▼", out w, out h);
            minimum = natural = w;
        } else {
            int w, h;
            base.measure ("9", out w, out h);
            minimum = natural = h;
        }
        minimum_baseline = natural_baseline = -1;
    }

    public override void query_data (GLib.Object lines, uint line) {
        Gtk.TextIter iter;
        buf.get_iter_at_line (out iter, (int) line);

        if (iter.get_chars_in_line () > 2000) {
            this.text = "";
            return;
        }

        var fold_tag = buf.tag_table.lookup ("folded");
        if (fold_tag != null && iter.has_tag (fold_tag)) {
            this.text = "";
            return;
        }

        Gtk.TextIter scan = iter;
        bool found = false;
        bool is_folded = false;
        int max_scan = 256;
        Gtk.TextIter open_bracket_iter = scan;
        
        while (!scan.ends_line () && !scan.is_end () && max_scan > 0) {
            unichar c = scan.get_char ();
            if ((c == '{' || c == '[') && !buf.iter_has_context_class (scan, "string")) {
                found = true;
                open_bracket_iter = scan;
                Gtk.TextIter next = scan;
                next.forward_char ();
                if (fold_tag != null && next.has_tag (fold_tag)) {
                    is_folded = true;
                }
                break;
            }
            scan.forward_char ();
            max_scan--;
        }
        
        if (found) {
            if (is_folded || is_foldable (open_bracket_iter)) {
                this.text = is_folded ? "▶" : "▼";
            } else {
                this.text = "";
            }
        } else {
            this.text = "";
        }
    }
}

public class EditorTab : Gtk.Box {
    public Gtk.Stack view_stack { get; private set; }
    public JsonColumnView column_view { get; private set; }
    public JsonGraphView graph_view { get; private set; }
    public string last_view_name { get; private set; default = "text"; }

    public GtkSource.View text_view { get; private set; }
    public GtkSource.Buffer text_buffer { get; private set; }
    public GLib.File? current_file { get; set; }
    public bool is_saving { get; set; default = false; }
    public bool is_loading { get; set; default = false; }
    
    public GtkSource.SearchSettings search_settings { get; private set; }
    public GtkSource.SearchContext search_context { get; private set; }

    public LineNumberGutterRenderer line_renderer { get; private set; }
    public FoldGutterRenderer fold_renderer { get; private set; }

    private uint64 last_mtime = 0;
    private uint32 last_mtime_usec = 0;
    private uint poll_timeout_id = 0;
    private Gtk.Box reload_banner;
    private Gtk.Box minify_banner;
    public signal void reload_requested ();

    public EditorTab () {
        GLib.Object (orientation: Gtk.Orientation.VERTICAL, spacing: 0);

        reload_banner = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 12);
        reload_banner.add_css_class ("background");
        reload_banner.margin_start = reload_banner.margin_end = 8;
        reload_banner.margin_top = reload_banner.margin_bottom = 8;
        
        var banner_label = new Gtk.Label (_("This file was modified by another program."));
        banner_label.hexpand = true;
        banner_label.xalign = 0.0f;
        reload_banner.append (banner_label);
        
        var reload_btn = new Gtk.Button.with_label (_("Reload"));
        reload_btn.clicked.connect (() => {
            reload_banner.visible = false;
            reload_requested ();
        });
        reload_banner.append (reload_btn);
        
        var banner_close_btn = new Gtk.Button.from_icon_name ("window-close-symbolic");
        banner_close_btn.add_css_class ("flat");
        banner_close_btn.clicked.connect (() => {
            reload_banner.visible = false;
        });
        reload_banner.append (banner_close_btn);

        reload_banner.visible = false;
        append (reload_banner);

        minify_banner = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 12);
        minify_banner.add_css_class ("background");
        minify_banner.margin_start = minify_banner.margin_end = 8;
        minify_banner.margin_top = minify_banner.margin_bottom = 8;
        
        var minify_label = new Gtk.Label (_("File is too large for single-line minification. Soft-minify (newlines added) applied to prevent UI crashes."));
        minify_label.hexpand = true;
        minify_label.xalign = 0.0f;
        minify_banner.append (minify_label);
        
        var minify_close_btn = new Gtk.Button.from_icon_name ("window-close-symbolic");
        minify_close_btn.add_css_class ("flat");
        minify_close_btn.clicked.connect (() => {
            minify_banner.visible = false;
        });
        minify_banner.append (minify_close_btn);

        minify_banner.visible = false;
        append (minify_banner);

        view_stack = new Gtk.Stack ();
        view_stack.transition_type = Gtk.StackTransitionType.CROSSFADE;
        view_stack.vexpand = true;

        text_buffer = new GtkSource.Buffer (null);
        text_buffer.enable_undo = true;
        
        var error_tag = new Gtk.TextTag ("error");
        error_tag.paragraph_background = "#770000";
        text_buffer.tag_table.add (error_tag);

        var fold_tag = new Gtk.TextTag ("folded");
        fold_tag.invisible = true;
        fold_tag.invisible_set = true;
        text_buffer.tag_table.add (fold_tag);

        search_settings = new GtkSource.SearchSettings ();
        search_settings.wrap_around = true;
        search_context = new GtkSource.SearchContext (text_buffer, search_settings);
        search_context.highlight = true;

        text_buffer.changed.connect (() => {
            Gtk.TextIter start, end;
            text_buffer.get_bounds (out start, out end);
            text_buffer.remove_tag_by_name ("error", start, end);
        });

        var scheme_manager = GtkSource.StyleSchemeManager.get_default ();
        var scheme = scheme_manager.get_scheme ("custom-dark");
        if (scheme == null) scheme = scheme_manager.get_scheme ("Adwaita-dark");
        if (scheme == null) scheme = scheme_manager.get_scheme ("oblivion");
        if (scheme != null) text_buffer.style_scheme = scheme;

        var lang_manager = GtkSource.LanguageManager.get_default ();
        var lang = lang_manager.get_language ("json");
        if (lang != null) text_buffer.language = lang;

        text_buffer.highlight_matching_brackets = true;

        var enc_tag = new Gtk.TextTag ("enclosing-bracket");
        Gdk.RGBA bg_color = Gdk.RGBA ();
        bg_color.parse ("#555555");
        enc_tag.background_rgba = bg_color;
        enc_tag.background_set = true;
        
        Gdk.RGBA fg_color = Gdk.RGBA ();
        fg_color.parse ("#00FFFF");
        enc_tag.foreground_rgba = fg_color;
        enc_tag.foreground_set = true;
        
        enc_tag.weight = Pango.Weight.HEAVY;
        enc_tag.weight_set = true;
        enc_tag.underline = Pango.Underline.SINGLE;
        enc_tag.underline_set = true;
        
        text_buffer.tag_table.add (enc_tag);
        enc_tag.set_priority (text_buffer.tag_table.get_size () - 1);

        text_view = new GtkSource.View.with_buffer (text_buffer);
        text_view.monospace = true;
        text_view.wrap_mode = Gtk.WrapMode.WORD_CHAR;
        text_view.show_line_numbers = false; // We use a custom renderer now
        text_view.highlight_current_line = false;
        text_view.auto_indent = true;
        text_view.indent_width = 4;
        text_view.insert_spaces_instead_of_tabs = true;
        text_view.add_css_class ("editor-view");

        var gutter = text_view.get_gutter (Gtk.TextWindowType.LEFT);
        line_renderer = new LineNumberGutterRenderer (text_buffer);
        gutter.insert (line_renderer, 10);
        fold_renderer = new FoldGutterRenderer (this);
        gutter.insert (fold_renderer, 20);

        var scrolled_window = new Gtk.ScrolledWindow ();
        scrolled_window.set_policy (Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC);
        scrolled_window.set_child (text_view);
        scrolled_window.vexpand = true;

        view_stack.add_titled (scrolled_window, "text", _("Text"));

        column_view = new JsonColumnView ();
        column_view.jump_to_text.connect (() => {
            view_stack.visible_child_name = "text";
        });
        view_stack.add_titled (column_view, "column", _("Columns"));

        graph_view = new JsonGraphView ();
        graph_view.jump_to_text.connect (() => {
            view_stack.visible_child_name = "text";
        });
        view_stack.add_titled (graph_view, "graph", _("Graph"));

        view_stack.notify["visible-child-name"].connect (() => {
            sync_views ();
            last_view_name = view_stack.visible_child_name;
        });

        append (view_stack);

        this.notify["current-file"].connect (() => {
            reset_monitor ();
        });

        text_buffer.notify["cursor-position"].connect (() => {
            update_enclosing_brackets ();
        });
    }

    public bool get_enclosing_brackets (out Gtk.TextIter start_bracket, out Gtk.TextIter end_bracket) {
        Gtk.TextIter cursor;
        text_buffer.get_iter_at_mark (out cursor, text_buffer.get_insert ());

        if (cursor.get_chars_in_line () > 2000) {
            start_bracket = cursor;
            end_bracket = cursor;
            return false;
        }
        
        Gtk.TextIter backward_iter = cursor;
        int brace_nesting = 0;
        int bracket_nesting = 0;
        bool found_open = false;
        unichar open_char = 0;
        int max_scan = 10000;
        
        while (backward_iter.backward_char () && max_scan > 0) {
            max_scan--;
            if (!text_buffer.iter_has_context_class (backward_iter, "string") && !text_buffer.iter_has_context_class (backward_iter, "comment")) {
                unichar c = backward_iter.get_char ();
                if (c == '}') brace_nesting++;
                else if (c == ']') bracket_nesting++;
                else if (c == '{') {
                    if (brace_nesting > 0) brace_nesting--;
                    else { found_open = true; open_char = '{'; break; }
                }
                else if (c == '[') {
                    if (bracket_nesting > 0) bracket_nesting--;
                    else { found_open = true; open_char = '['; break; }
                }
            }
        }
        
        if (!found_open) {
            start_bracket = cursor;
            end_bracket = cursor;
            return false;
        }
        
        Gtk.TextIter forward_iter = cursor;
        brace_nesting = 0;
        bracket_nesting = 0;
        bool found_close = false;
        unichar target_close = (open_char == '{') ? '}' : ']';
        max_scan = 10000;
        
        while (!forward_iter.is_end () && max_scan > 0) {
            max_scan--;
            unichar c = forward_iter.get_char ();
            if (!text_buffer.iter_has_context_class (forward_iter, "string") && !text_buffer.iter_has_context_class (forward_iter, "comment")) {
                if (c == '{') brace_nesting++;
                else if (c == '[') bracket_nesting++;
                else if (c == '}') {
                    if (brace_nesting > 0) brace_nesting--;
                    else if (target_close == '}') { found_close = true; break; }
                }
                else if (c == ']') {
                    if (bracket_nesting > 0) bracket_nesting--;
                    else if (target_close == ']') { found_close = true; break; }
                }
            }
            forward_iter.forward_char ();
        }
        
        if (found_close) {
            start_bracket = backward_iter;
            end_bracket = forward_iter;
            return true;
        }
        start_bracket = cursor;
        end_bracket = cursor;
        return false;
    }

    private void update_enclosing_brackets () {
        Gtk.TextIter start_buf, end_buf;
        text_buffer.get_bounds (out start_buf, out end_buf);
        text_buffer.remove_tag_by_name ("enclosing-bracket", start_buf, end_buf);
        
        Gtk.TextIter open_bracket, close_bracket;
        if (get_enclosing_brackets (out open_bracket, out close_bracket)) {
            Gtk.TextIter open_end = open_bracket;
            open_end.forward_char ();
            text_buffer.apply_tag_by_name ("enclosing-bracket", open_bracket, open_end);
            
            Gtk.TextIter close_end = close_bracket;
            close_end.forward_char ();
            text_buffer.apply_tag_by_name ("enclosing-bracket", close_bracket, close_end);
        }
    }

    public void toggle_fold_at (Gtk.TextIter open_bracket) {
        Gtk.TextIter forward_iter = open_bracket;
        unichar open_char = forward_iter.get_char ();
        if (open_char != '{' && open_char != '[') return;
        
        unichar target_close = (open_char == '{') ? '}' : ']';
        int nesting = 0;
        bool found_close = false;
        int max_scan = 50000;
        
        while (!forward_iter.is_end () && max_scan > 0) {
            max_scan--;
            unichar c = forward_iter.get_char ();
            if (!text_buffer.iter_has_context_class (forward_iter, "string") && !text_buffer.iter_has_context_class (forward_iter, "comment")) {
                if (c == open_char) {
                    nesting++;
                } else if (c == target_close) {
                    nesting--;
                    if (nesting == 0) {
                        found_close = true;
                        break;
                    }
                }
            }
            forward_iter.forward_char ();
        }
        
        if (found_close) {
            if (open_bracket.get_line () == forward_iter.get_line ()) return;
            
            Gtk.TextIter fold_start = open_bracket;
            fold_start.forward_char ();
            Gtk.TextIter fold_end = forward_iter;
            
            if (fold_start.compare (fold_end) >= 0) return;
            
            if (fold_start.has_tag (text_buffer.tag_table.lookup ("folded"))) {
                text_buffer.remove_tag_by_name ("folded", fold_start, fold_end);
            } else {
                text_buffer.apply_tag_by_name ("folded", fold_start, fold_end);
            }
        }
    }

    public void toggle_fold () {
        Gtk.TextIter cursor;
        text_buffer.get_iter_at_mark (out cursor, text_buffer.get_insert ());
        
        unichar c = cursor.get_char ();
        if ((c == '{' || c == '[') && !text_buffer.iter_has_context_class (cursor, "string")) {
            toggle_fold_at (cursor);
            return;
        }
        
        Gtk.TextIter open_bracket, close_bracket;
        if (get_enclosing_brackets (out open_bracket, out close_bracket)) {
            toggle_fold_at (open_bracket);
        }
    }

    public void sync_views () {
        if (view_stack.visible_child_name != "text") {
            Gtk.TextIter start, end;
            text_buffer.get_bounds (out start, out end);
            string text = text_buffer.get_text (start, end, false);
            if (view_stack.visible_child_name == "column") {
                column_view.load_json (text);
            } else if (view_stack.visible_child_name == "graph") {
                graph_view.load_json (text);
            }
        } else if (view_stack.visible_child_name == "text") {
            if (last_view_name == "column" && column_view.user_clicked_column && column_view.last_loaded_text.length > 0) {
                if (column_view.selected_offset <= column_view.last_loaded_text.length) {
                    string prefix = column_view.last_loaded_text.substring (0, (long) column_view.selected_offset);
                    int char_offset = prefix.char_count ();
                    
                    Gtk.TextIter iter;
                    text_buffer.get_iter_at_offset (out iter, char_offset);
                    text_buffer.place_cursor (iter);
                    GLib.Timeout.add (50, () => {
                        if (view_stack.transition_running) return GLib.Source.CONTINUE;
                        text_view.scroll_to_mark (text_buffer.get_insert (), 0.0, true, 0.5, 0.5);
                        text_view.grab_focus ();
                        return GLib.Source.REMOVE;
                    });
                } else {
                    GLib.Timeout.add (50, () => { 
                        if (view_stack.transition_running) return GLib.Source.CONTINUE;
                        text_view.grab_focus (); 
                        return GLib.Source.REMOVE; 
                    });
                }
                column_view.user_clicked_column = false;
            } else if (last_view_name == "graph" && graph_view.user_clicked_node && graph_view.last_loaded_text.length > 0) {
                if (graph_view.selected_offset <= graph_view.last_loaded_text.length) {
                    string prefix = graph_view.last_loaded_text.substring (0, (long) graph_view.selected_offset);
                    int char_offset = prefix.char_count ();
                    
                    Gtk.TextIter iter;
                    text_buffer.get_iter_at_offset (out iter, char_offset);
                    text_buffer.place_cursor (iter);
                    GLib.Timeout.add (50, () => {
                        if (view_stack.transition_running) return GLib.Source.CONTINUE;
                        text_view.scroll_to_mark (text_buffer.get_insert (), 0.0, true, 0.5, 0.5);
                        text_view.grab_focus ();
                        return GLib.Source.REMOVE;
                    });
                } else {
                    GLib.Timeout.add (50, () => { 
                        if (view_stack.transition_running) return GLib.Source.CONTINUE;
                        text_view.grab_focus (); 
                        return GLib.Source.REMOVE; 
                    });
                }
                graph_view.user_clicked_node = false;
            } else {
                GLib.Timeout.add (50, () => { 
                    if (view_stack.transition_running) return GLib.Source.CONTINUE;
                    text_view.grab_focus (); 
                    return GLib.Source.REMOVE; 
                });
            }
        }
    }

    public string get_tab_title () {
        string prefix = text_buffer.get_modified () ? "*" : "";
        if (current_file != null) {
            return prefix + current_file.get_basename ();
        }
        return prefix + "Untitled.json";
    }

    public void hide_reload_banner () {
        reload_banner.visible = false;
    }

    public void show_minify_banner (bool show) {
        minify_banner.visible = show;
    }

    public override void dispose () {
        if (poll_timeout_id != 0) {
            GLib.Source.remove (poll_timeout_id);
            poll_timeout_id = 0;
        }
        base.dispose ();
    }

    public void reset_monitor () {
        if (poll_timeout_id != 0) {
            GLib.Source.remove (poll_timeout_id);
            poll_timeout_id = 0;
        }
        if (current_file != null) {
            try {
                var info = current_file.query_info ("time::modified,time::modified-usec", GLib.FileQueryInfoFlags.NONE, null);
                last_mtime = info.get_attribute_uint64 ("time::modified");
                last_mtime_usec = info.get_attribute_uint32 ("time::modified-usec");
            } catch (GLib.Error e) {
                last_mtime = 0;
                last_mtime_usec = 0;
            }
            poll_timeout_id = GLib.Timeout.add_seconds (1, () => {
                if (is_saving || current_file == null || reload_banner.visible) return GLib.Source.CONTINUE;
                try {
                    var info = current_file.query_info ("time::modified,time::modified-usec", GLib.FileQueryInfoFlags.NONE, null);
                    uint64 current_mtime = info.get_attribute_uint64 ("time::modified");
                    uint32 current_mtime_usec = info.get_attribute_uint32 ("time::modified-usec");
                    
                    if (last_mtime != 0) {
                        if (current_mtime > last_mtime || (current_mtime == last_mtime && current_mtime_usec > last_mtime_usec)) {
                            reload_banner.visible = true;
                        }
                    } else if (last_mtime == 0 && current_mtime != 0) {
                        last_mtime = current_mtime;
                        last_mtime_usec = current_mtime_usec;
                    }
                } catch (GLib.Error e) {
                }
                return GLib.Source.CONTINUE;
            });
        }
    }
}

public class EditorWindow : Gtk.ApplicationWindow {

    private Gtk.Notebook notebook;
    private Gtk.HeaderBar header_bar;
    private Gtk.StackSwitcher view_switcher;
    private Gtk.Spinner busy_spinner;

    private Gtk.Label status_left_label;
    private Gtk.Label status_path_label;
    private Gtk.Label status_cursor_label;
    private Gtk.MenuButton status_zoom_button;
    private GLib.Menu recent_menu;
    private GLib.Menu tab_menu_model;

    private Gtk.Revealer search_revealer;
    private Gtk.SearchEntry search_entry;
    private Gtk.Label search_count_label;
    private Gtk.Entry replace_entry;

    private Gtk.CssProvider zoom_provider;
    private int current_font_size = 12;
    private bool setting_word_wrap = true;
    private bool setting_show_line_numbers = true;
    private bool setting_allow_comments = false;
    private int active_tab_index = 0;
    private int pending_loads = 0;

    public const string ACTION_NEW = "win.new_tab";
    public const string ACTION_OPEN = "win.open";
    public const string ACTION_SAVE = "win.save";
    public const string ACTION_SAVE_AS = "win.save_as";
    public const string ACTION_QUIT = "app.quit";
    public const string ACTION_ZOOM_IN = "win.zoom_in";
    public const string ACTION_ZOOM_OUT = "win.zoom_out";
    public const string ACTION_ZOOM_RESET = "win.zoom_reset";

    public EditorWindow (Gtk.Application app, bool restore_session = true) {
        GLib.Object (application: app);

        Gtk.Settings.get_default ().gtk_application_prefer_dark_theme = true;
        GLib.Environment.set_application_name ("JsonLens");

        var scheme_manager = GtkSource.StyleSchemeManager.get_default ();
        string base_scheme = "oblivion";
        if (scheme_manager.get_scheme ("Adwaita-dark") != null) {
            base_scheme = "Adwaita-dark";
        }
        
        string style_dir = GLib.Path.build_filename (GLib.Environment.get_user_config_dir (), "json-lens", "styles");
        GLib.DirUtils.create_with_parents (style_dir, 0755);
        string style_file = GLib.Path.build_filename (style_dir, "custom-dark.xml");
        
        string xml = """<?xml version="1.0" encoding="UTF-8"?>
<style-scheme id="custom-dark" name="Custom Dark" version="1.0" parent-scheme="%s">
  <author>JsonLens</author>
  <description>Custom scheme</description>
  <style name="bracket-match" background="#555555" foreground="#00FFFF" bold="true" underline="true"/>
  <style name="bracket-mismatch" background="#550000" foreground="#FF0000" bold="true"/>
</style-scheme>""".printf(base_scheme);

        try { GLib.FileUtils.set_contents (style_file, xml); } catch (GLib.Error e) {}
        
        scheme_manager.append_search_path (style_dir);
        scheme_manager.force_rescan ();

        FileState[] files_to_open = new FileState[0];
        if (restore_session) {
            files_to_open = load_settings ();
        }

        set_default_size (800, 600);
        update_title ();

        header_bar = new Gtk.HeaderBar ();
        set_titlebar (header_bar);

        view_switcher = new Gtk.StackSwitcher ();
        header_bar.set_title_widget (view_switcher);

        busy_spinner = new Gtk.Spinner();
        header_bar.pack_end(busy_spinner);

        var menubar_model = new GLib.Menu ();

        var file_menu = new GLib.Menu ();
        file_menu.append ( _("New Tab"), ACTION_NEW);
        file_menu.append ( _("Open"), ACTION_OPEN);
        file_menu.append ( _("Save"), ACTION_SAVE);
        file_menu.append ( _("Save As..."), ACTION_SAVE_AS);
        recent_menu = new GLib.Menu ();
        file_menu.append_submenu (_("Recent Files"), recent_menu);
        var quit_section = new GLib.Menu ();
        quit_section.append ( _("Quit"), ACTION_QUIT);
        file_menu.append_section (null, quit_section);

        var edit_menu = new GLib.Menu ();
        var history_section = new GLib.Menu ();
        history_section.append (_("Undo"), "win.undo");
        history_section.append (_("Redo"), "win.redo");
        edit_menu.append_section (null, history_section);
        var clipboard_section = new GLib.Menu ();
        clipboard_section.append (_("Cut"), "win.cut");
        clipboard_section.append (_("Copy"), "win.copy");
        clipboard_section.append (_("Paste"), "win.paste");
        clipboard_section.append (_("Select All"), "win.select_all");
        edit_menu.append_section (null, clipboard_section);
        var search_section = new GLib.Menu ();
        search_section.append (_("Find"), "win.find");
        search_section.append (_("Replace"), "win.replace");
        edit_menu.append_section (null, search_section);
        var json_section = new GLib.Menu ();
        json_section.append (_("Format JSON"), "win.format_json");
        json_section.append (_("Minify JSON"), "win.minify_json");
        edit_menu.append_section (null, json_section);

        var view_menu = new GLib.Menu ();
        view_menu.append (_("Toggle Fold"), "win.toggle_fold");
        view_menu.append (_("Word Wrap"), "win.toggle_word_wrap");
        view_menu.append (_("Line Numbers"), "win.toggle_line_numbers");
        view_menu.append (_("Allow JSON Comments"), "win.toggle_allow_comments");

        menubar_model.append_submenu (_("File"), file_menu);
        menubar_model.append_submenu (_("Edit"), edit_menu);
        menubar_model.append_submenu (_("View"), view_menu);

        tab_menu_model = new GLib.Menu ();
        tab_menu_model.append (_("Move Left"), "tab.move_left");
        tab_menu_model.append (_("Move Right"), "tab.move_right");
        tab_menu_model.append (_("Move to New Window"), "tab.move_new_window");
        var close_section = new GLib.Menu ();
        close_section.append (_("Close Other Tabs"), "tab.close_others");
        close_section.append (_("Close Tab"), "tab.close");
        tab_menu_model.append_section (null, close_section);

        var menu_bar = new Gtk.PopoverMenuBar.from_model (menubar_model);
        header_bar.pack_start (menu_bar);

        var new_action = new GLib.SimpleAction ("new_tab", null);
        new_action.activate.connect (() => {
            create_new_tab ();
        });
        this.add_action (new_action);

        var open_action = new GLib.SimpleAction ("open", null);
        open_action.activate.connect (this.on_open_button_clicked);
        this.add_action (open_action);

        var save_action = new GLib.SimpleAction ("save", null);
        save_action.activate.connect (this.on_save_button_clicked);
        this.add_action (save_action);

        var save_as_action = new GLib.SimpleAction ("save_as", null);
        save_as_action.activate.connect (this.on_save_as_button_clicked);
        this.add_action (save_as_action);

        var open_recent_action = new GLib.SimpleAction ("open_recent", new GLib.VariantType ("s"));
        open_recent_action.activate.connect ((action, parameter) => {
            if (parameter != null) {
                string uri = parameter.get_string ();
                var file = GLib.File.new_for_uri (uri);
                this.do_open_file_async.begin (file);
            }
        });
        this.add_action (open_recent_action);

        var undo_action = new GLib.SimpleAction ("undo", null);
        undo_action.activate.connect (() => {
            var tab = get_current_tab ();
            if (tab != null && tab.text_buffer.get_can_undo ()) tab.text_buffer.undo ();
        });
        this.add_action (undo_action);

        var redo_action = new GLib.SimpleAction ("redo", null);
        redo_action.activate.connect (() => {
            var tab = get_current_tab ();
            if (tab != null && tab.text_buffer.get_can_redo ()) tab.text_buffer.redo ();
        });
        this.add_action (redo_action);

        var cut_action = new GLib.SimpleAction ("cut", null);
        cut_action.activate.connect (() => {
            var tab = get_current_tab ();
            if (tab != null) tab.text_buffer.cut_clipboard (tab.text_view.get_clipboard (), true);
        });
        this.add_action (cut_action);

        var copy_action = new GLib.SimpleAction ("copy", null);
        copy_action.activate.connect (() => {
            var tab = get_current_tab ();
            if (tab != null) tab.text_buffer.copy_clipboard (tab.text_view.get_clipboard ());
        });
        this.add_action (copy_action);

        var paste_action = new GLib.SimpleAction ("paste", null);
        paste_action.activate.connect (() => {
            var tab = get_current_tab ();
            if (tab != null) tab.text_buffer.paste_clipboard (tab.text_view.get_clipboard (), null, true);
        });
        this.add_action (paste_action);

        var select_all_action = new GLib.SimpleAction ("select_all", null);
        select_all_action.activate.connect (() => {
            var tab = get_current_tab ();
            if (tab != null) {
                Gtk.TextIter start, end;
                tab.text_buffer.get_bounds (out start, out end);
                tab.text_buffer.select_range (start, end);
            }
        });
        this.add_action (select_all_action);

        var find_action = new GLib.SimpleAction ("find", null);
        find_action.activate.connect (() => {
            search_revealer.reveal_child = true;
            search_entry.grab_focus ();
        });
        this.add_action (find_action);

        var replace_action = new GLib.SimpleAction ("replace", null);
        replace_action.activate.connect (() => {
            search_revealer.reveal_child = true;
            replace_entry.grab_focus ();
        });
        this.add_action (replace_action);

        var format_action = new GLib.SimpleAction ("format_json", null);
        format_action.activate.connect (() => {
            var tab = get_current_tab ();
            if (tab != null) do_format_action.begin (tab, true);
        });
        this.add_action (format_action);

        var minify_action = new GLib.SimpleAction ("minify_json", null);
        minify_action.activate.connect (() => {
            var tab = get_current_tab ();
            if (tab != null) do_format_action.begin (tab, false);
        });
        this.add_action (minify_action);

        var fold_action = new GLib.SimpleAction ("toggle_fold", null);
        fold_action.activate.connect (() => {
            var tab = get_current_tab ();
            if (tab != null) tab.toggle_fold ();
        });
        this.add_action (fold_action);

        var zoom_in_action = new GLib.SimpleAction ("zoom_in", null);
        zoom_in_action.activate.connect (() => {
            current_font_size += 1;
            update_zoom ();
        });
        this.add_action (zoom_in_action);

        var zoom_out_action = new GLib.SimpleAction ("zoom_out", null);
        zoom_out_action.activate.connect (() => {
            if (current_font_size > 4) {
                current_font_size -= 1;
                update_zoom ();
            }
        });
        this.add_action (zoom_out_action);

        var zoom_reset_action = new GLib.SimpleAction ("zoom_reset", null);
        zoom_reset_action.activate.connect (() => {
            current_font_size = 12;
            update_zoom ();
        });
        this.add_action (zoom_reset_action);

        var wrap_action = new GLib.SimpleAction.stateful ("toggle_word_wrap", null, new GLib.Variant.boolean (setting_word_wrap));
        wrap_action.change_state.connect ((action, state) => {
            setting_word_wrap = state.get_boolean ();
            action.set_state (state);
            for (int i = 0; i < notebook.get_n_pages (); i++) {
                var t = notebook.get_nth_page (i) as EditorTab;
                if (t != null) t.text_view.wrap_mode = setting_word_wrap ? Gtk.WrapMode.WORD_CHAR : Gtk.WrapMode.NONE;
            }
            save_settings ();
        });
        this.add_action (wrap_action);

        var lines_action = new GLib.SimpleAction.stateful ("toggle_line_numbers", null, new GLib.Variant.boolean (setting_show_line_numbers));
        lines_action.change_state.connect ((action, state) => {
            setting_show_line_numbers = state.get_boolean ();
            action.set_state (state);
            for (int i = 0; i < notebook.get_n_pages (); i++) {
                var t = notebook.get_nth_page (i) as EditorTab;
                if (t != null) t.line_renderer.visible = setting_show_line_numbers;
            }
            save_settings ();
        });
        this.add_action (lines_action);

        var comments_action = new GLib.SimpleAction.stateful ("toggle_allow_comments", null, new GLib.Variant.boolean (setting_allow_comments));
        comments_action.change_state.connect ((action, state) => {
            setting_allow_comments = state.get_boolean ();
            action.set_state (state);
            save_settings ();
        });
        this.add_action (comments_action);

        var tab_left_action = new GLib.SimpleAction ("tab_move_left", null);
        tab_left_action.activate.connect (() => {
            var target = get_current_tab ();
            if (target == null) return;
            int page_num = notebook.page_num (target);
            if (page_num > 0) {
                notebook.reorder_child (target, page_num - 1);
                save_settings ();
            }
        });
        this.add_action (tab_left_action);

        var tab_right_action = new GLib.SimpleAction ("tab_move_right", null);
        tab_right_action.activate.connect (() => {
            var target = get_current_tab ();
            if (target == null) return;
            int page_num = notebook.page_num (target);
            if (page_num >= 0 && page_num < notebook.get_n_pages () - 1) {
                notebook.reorder_child (target, page_num + 1);
                save_settings ();
            }
        });
        this.add_action (tab_right_action);

        var tab_new_win_action = new GLib.SimpleAction ("tab_move_new_window", null);
        tab_new_win_action.activate.connect (() => {
            var target = get_current_tab ();
            if (target == null || notebook.get_n_pages () <= 1) return;
            
            var app_ref = this.application as EditorApplication;
            var new_win = new EditorWindow (app_ref, false);
            var new_tab = new_win.get_current_tab ();
            
            if (target.current_file != null) new_tab.current_file = target.current_file;
            Gtk.TextIter start, end;
            target.text_buffer.get_bounds (out start, out end);
            new_tab.text_buffer.set_text (target.text_buffer.get_text (start, end, false), -1);
            new_win.update_title ();
            new_win.present ();
            
            this.activate_action ("win.tab_close", null);
        });
        this.add_action (tab_new_win_action);

        var tab_close_others_action = new GLib.SimpleAction ("tab_close_others", null);
        tab_close_others_action.activate.connect (() => {
            var target = get_current_tab ();
            if (target == null) return;
            for (int i = notebook.get_n_pages () - 1; i >= 0; i--) {
                var t = notebook.get_nth_page (i) as EditorTab;
                if (t != target) notebook.remove_page (i);
            }
            save_settings ();
        });
        this.add_action (tab_close_others_action);

        var tab_close_action = new GLib.SimpleAction ("tab_close", null);
        tab_close_action.activate.connect (() => {
            var target = get_current_tab ();
            if (target == null) return;
            int page_num = notebook.page_num (target);
            if (page_num >= 0) {
                notebook.remove_page (page_num);
                if (notebook.get_n_pages () == 0) create_new_tab ();
                save_settings ();
            }
        });
        this.add_action (tab_close_action);

        zoom_provider = new Gtk.CssProvider ();
        add_provider_for_display (Gdk.Display.get_default (), zoom_provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION);

        var main_box = new Gtk.Box (Gtk.Orientation.VERTICAL, 0);

        notebook = new Gtk.Notebook ();
        notebook.scrollable = true;
        notebook.enable_popup = false;
        notebook.vexpand = true;
        notebook.switch_page.connect ((page, page_num) => {
            GLib.Idle.add (() => {
                update_title ();
                update_cursor_status ();
                update_search_count ();
                save_settings ();
                var tab = page as EditorTab;
                if (tab != null && view_switcher != null) {
                    view_switcher.set_stack (tab.view_stack);
                    tab.sync_views ();
                }
                return GLib.Source.REMOVE;
            });
        });

        search_revealer = new Gtk.Revealer ();
        search_revealer.transition_type = Gtk.RevealerTransitionType.SLIDE_DOWN;

        var search_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 6);
        search_box.margin_start = search_box.margin_end = search_box.margin_top = search_box.margin_bottom = 6;

        search_entry = new Gtk.SearchEntry ();
        search_entry.hexpand = true;

        search_count_label = new Gtk.Label ("");
        search_count_label.add_css_class ("dim-label");
        search_count_label.margin_start = 6;
        search_count_label.margin_end = 6;
        
        var next_btn = new Gtk.Button.from_icon_name ("go-down-symbolic");
        var prev_btn = new Gtk.Button.from_icon_name ("go-up-symbolic");
        
        replace_entry = new Gtk.Entry ();
        replace_entry.placeholder_text = _("Replace with...");
        replace_entry.hexpand = true;
        
        var replace_btn = new Gtk.Button.with_label (_("Replace"));
        var replace_all_btn = new Gtk.Button.with_label (_("Replace All"));
        var close_search_btn = new Gtk.Button.from_icon_name ("window-close-symbolic");
        close_search_btn.add_css_class ("flat");

        search_entry.search_changed.connect (() => {
            string text = search_entry.get_text ();
            for (int i = 0; i < notebook.get_n_pages (); i++) {
                var t = notebook.get_nth_page (i) as EditorTab;
                if (t != null) t.search_settings.search_text = text;
            }
            update_search_count ();
        });

        search_entry.stop_search.connect (() => { close_search_btn.clicked (); });
        search_entry.activate.connect (() => { next_btn.clicked (); });
        replace_entry.activate.connect (() => { replace_btn.clicked (); });

        next_btn.clicked.connect (() => {
            var tab = get_current_tab ();
            if (tab == null) return;
            string text = search_entry.get_text ();
            if (text.length == 0) return;
            
            Gtk.TextIter start_sel, end_sel, iter;
            if (tab.text_buffer.get_selection_bounds (out start_sel, out end_sel)) {
                iter = end_sel;
            } else {
                tab.text_buffer.get_iter_at_mark (out iter, tab.text_buffer.get_insert ());
            }
            Gtk.TextIter match_start, match_end;
            if (iter.forward_search (text, Gtk.TextSearchFlags.CASE_INSENSITIVE, out match_start, out match_end, null)) {
                tab.text_buffer.select_range (match_start, match_end);
                tab.text_view.scroll_to_mark (tab.text_buffer.get_insert (), 0.0, false, 0.0, 0.0);
            } else {
                Gtk.TextIter start;
                tab.text_buffer.get_start_iter (out start);
                if (start.forward_search (text, Gtk.TextSearchFlags.CASE_INSENSITIVE, out match_start, out match_end, null)) {
                    tab.text_buffer.select_range (match_start, match_end);
                    tab.text_view.scroll_to_mark (tab.text_buffer.get_insert (), 0.0, false, 0.0, 0.0);
                }
            }
        });

        prev_btn.clicked.connect (() => {
            var tab = get_current_tab ();
            if (tab == null) return;
            string text = search_entry.get_text ();
            if (text.length == 0) return;
            
            Gtk.TextIter start_sel, end_sel, iter;
            if (tab.text_buffer.get_selection_bounds (out start_sel, out end_sel)) {
                iter = start_sel;
            } else {
                tab.text_buffer.get_iter_at_mark (out iter, tab.text_buffer.get_insert ());
            }
            Gtk.TextIter match_start, match_end;
            if (iter.backward_search (text, Gtk.TextSearchFlags.CASE_INSENSITIVE, out match_start, out match_end, null)) {
                tab.text_buffer.select_range (match_start, match_end);
                tab.text_view.scroll_to_mark (tab.text_buffer.get_insert (), 0.0, false, 0.0, 0.0);
            } else {
                Gtk.TextIter end;
                tab.text_buffer.get_end_iter (out end);
                if (end.backward_search (text, Gtk.TextSearchFlags.CASE_INSENSITIVE, out match_start, out match_end, null)) {
                    tab.text_buffer.select_range (match_start, match_end);
                    tab.text_view.scroll_to_mark (tab.text_buffer.get_insert (), 0.0, false, 0.0, 0.0);
                }
            }
        });

        replace_btn.clicked.connect (() => {
            var tab = get_current_tab ();
            if (tab == null) return;
            string search_txt = search_entry.get_text ().down ();
            if (search_txt.length == 0) return;
            
            Gtk.TextIter start, end;
            bool has_sel = tab.text_buffer.get_selection_bounds (out start, out end);
            string selected = has_sel ? tab.text_buffer.get_text (start, end, false).down () : "";
            
            if (!has_sel || selected != search_txt) {
                next_btn.clicked ();
                has_sel = tab.text_buffer.get_selection_bounds (out start, out end);
                selected = has_sel ? tab.text_buffer.get_text (start, end, false).down () : "";
                if (!has_sel || selected != search_txt) return;
            }
            
            tab.text_buffer.begin_user_action ();
            tab.text_buffer.delete (ref start, ref end);
            tab.text_buffer.insert (ref start, replace_entry.get_text (), -1);
            tab.text_buffer.end_user_action ();
            tab.text_buffer.place_cursor (start);
            next_btn.clicked ();
        });

        replace_all_btn.clicked.connect (() => {
            var tab = get_current_tab ();
            if (tab == null) return;
            string search_txt = search_entry.get_text ();
            string replace_txt = replace_entry.get_text ();
            if (search_txt.length == 0) return;
            
            tab.text_buffer.begin_user_action ();
            Gtk.TextIter iter;
            tab.text_buffer.get_start_iter (out iter);
            Gtk.TextIter match_start, match_end;
            while (iter.forward_search (search_txt, Gtk.TextSearchFlags.CASE_INSENSITIVE, out match_start, out match_end, null)) {
                tab.text_buffer.delete (ref match_start, ref match_end);
                tab.text_buffer.insert (ref match_start, replace_txt, -1);
                iter = match_start; 
            }
            tab.text_buffer.end_user_action ();
        });

        close_search_btn.clicked.connect (() => {
            search_revealer.reveal_child = false;
            search_entry.set_text ("");
            search_count_label.label = "";
            var tab = get_current_tab ();
            if (tab != null) tab.text_view.grab_focus ();
        });

        search_box.append (search_entry);
        search_box.append (search_count_label);
        search_box.append (prev_btn);
        search_box.append (next_btn);
        search_box.append (replace_entry);
        search_box.append (replace_btn);
        search_box.append (replace_all_btn);
        search_box.append (close_search_btn);

        search_revealer.set_child (search_box);

        status_left_label = new Gtk.Label (_("Ready"));
        status_left_label.xalign = 0.0f;
        status_left_label.hexpand = false;
        status_left_label.margin_start = 6;
        status_left_label.margin_end = 6;
        status_left_label.margin_top = 4;
        status_left_label.margin_bottom = 4;
        
        status_path_label = new Gtk.Label ("");
        status_path_label.hexpand = true;
        status_path_label.ellipsize = Pango.EllipsizeMode.MIDDLE;
        status_path_label.margin_start = 6;
        status_path_label.margin_end = 6;
        status_path_label.add_css_class ("dim-label");

        status_cursor_label = new Gtk.Label ("Ln 1, Col 1");
        status_cursor_label.margin_start = 6;
        status_cursor_label.margin_end = 6;
        status_cursor_label.margin_top = 4;
        status_cursor_label.margin_bottom = 4;

        var separator = new Gtk.Separator (Gtk.Orientation.VERTICAL);
        separator.margin_top = 4;
        separator.margin_bottom = 4;

        status_zoom_button = new Gtk.MenuButton ();
        status_zoom_button.has_frame = false;
        status_zoom_button.label = "Font Size: 12pt";
        status_zoom_button.margin_start = 6;
        status_zoom_button.margin_end = 6;

        var zoom_popover = new Gtk.Popover ();
        var zoom_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 6);
        zoom_box.margin_start = zoom_box.margin_end = zoom_box.margin_top = zoom_box.margin_bottom = 6;
        
        var zoom_spin = new Gtk.SpinButton.with_range (4.0, 72.0, 1.0);
        zoom_spin.value = current_font_size;
        zoom_spin.value_changed.connect (() => {
            current_font_size = (int) zoom_spin.value;
            update_zoom ();
        });
        
        zoom_box.append (new Gtk.Label (_("Size:")));
        zoom_box.append (zoom_spin);
        zoom_popover.set_child (zoom_box);
        
        status_zoom_button.popover = zoom_popover;
        status_zoom_button.notify["active"].connect (() => {
            if (status_zoom_button.active) zoom_spin.value = current_font_size;
        });

        var status_bar = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 0);
        status_bar.append (status_left_label);
        status_bar.append (status_path_label);
        status_bar.append (status_cursor_label);
        status_bar.append (separator);
        status_bar.append (status_zoom_button);

        main_box.append (search_revealer);
        main_box.append (notebook);
        main_box.append (status_bar);

        set_child (main_box);
        
        update_recent_menu ();

        update_zoom ();

        var drop_target = new Gtk.DropTarget (typeof (Gdk.FileList), Gdk.DragAction.COPY);
        drop_target.drop.connect ((value, x, y) => {
            var file_list = (Gdk.FileList) value.get_boxed ();
            if (file_list != null) {
                var files = file_list.get_files ();
                foreach (var file in files) {
                    this.do_open_file_async.begin (file, null, false);
                }
                return true;
            }
            return false;
        });
        ((Gtk.Widget) this).add_controller (drop_target);

        this.close_request.connect (() => {
            int unsaved = 0;
            for (int i = 0; i < notebook.get_n_pages (); i++) {
                var t = notebook.get_nth_page (i) as EditorTab;
                if (t != null && t.text_buffer.get_modified ()) unsaved++;
            }
            if (unsaved > 0) {
                var dialog = new Gtk.AlertDialog (_("You have %d unsaved files. Close anyway?").printf(unsaved));
                dialog.buttons = {_("Cancel"), _("Close Without Saving")};
                dialog.cancel_button = 0;
                dialog.choose.begin (this, null, (obj, res) => {
                    try {
                        if (dialog.choose.end (res) == 1) {
                            for (int i = 0; i < notebook.get_n_pages (); i++) {
                                var t = notebook.get_nth_page (i) as EditorTab;
                                if (t != null) t.text_buffer.set_modified (false);
                            }
                            this.close ();
                        }
                    } catch (GLib.Error e) {}
                });
                return true;
            }
            save_settings ();
            return false;
        });

        if (files_to_open.length > 0) {
            foreach (var fs in files_to_open) {
                var file = GLib.File.new_for_uri (fs.uri);
                var tab = create_new_tab ();
                this.do_open_file_async.begin (file, tab, false, fs);
            }
            if (active_tab_index >= 0 && active_tab_index < notebook.get_n_pages ()) {
                notebook.set_current_page (active_tab_index);
            }
        } else {
            create_new_tab ();
        }
    }

    private EditorTab create_new_tab () {
        var tab = new EditorTab ();
        
        tab.text_view.wrap_mode = setting_word_wrap ? Gtk.WrapMode.WORD_CHAR : Gtk.WrapMode.NONE;
        tab.line_renderer.visible = setting_show_line_numbers;

        tab.search_settings.search_text = search_entry != null ? search_entry.get_text () : "";
        
        var tab_box = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 4);
        var label = new Gtk.Label (tab.get_tab_title ());
        var close_btn = new Gtk.Button.from_icon_name ("window-close-symbolic");
        close_btn.add_css_class ("flat");
        tab_box.append (label);
        tab_box.append (close_btn);
        
        tab.notify["current-file"].connect (() => {
            label.label = tab.get_tab_title ();
            notebook.set_menu_label_text (tab, tab.get_tab_title ());
        });
        
        tab.reload_requested.connect (() => {
            if (tab.current_file != null) {
                var fresh_file = GLib.File.new_for_uri (tab.current_file.get_uri ());
                this.do_open_file_async.begin (fresh_file, tab, true);
            }
        });
        
        close_btn.clicked.connect (() => {
            if (tab.text_buffer.get_modified ()) {
                var dialog = new Gtk.AlertDialog (_("This file has unsaved changes. Discard?"));
                dialog.buttons = {_("Cancel"), _("Discard")};
                dialog.cancel_button = 0;
                dialog.choose.begin (this, null, (obj, res) => {
                    try {
                        if (dialog.choose.end (res) == 1) force_close_tab (tab);
                    } catch (GLib.Error e) {}
                });
            } else {
                force_close_tab (tab);
            }
        });
        
        tab.text_buffer.modified_changed.connect (() => {
            label.label = tab.get_tab_title ();
            notebook.set_menu_label_text (tab, tab.get_tab_title ());
            if (get_current_tab () == tab) update_title ();
        });

        tab.text_buffer.notify["cursor-position"].connect (update_cursor_status);

        tab.search_context.notify["occurrences-count"].connect (() => {
            if (get_current_tab () == tab) update_search_count ();
        });

        var tab_actions = new GLib.SimpleActionGroup ();
        
        var left_action = new GLib.SimpleAction ("move_left", null);
        left_action.activate.connect (() => {
            int page_num = notebook.page_num (tab);
            if (page_num > 0) { notebook.reorder_child (tab, page_num - 1); save_settings (); }
        });
        tab_actions.add_action (left_action);

        var right_action = new GLib.SimpleAction ("move_right", null);
        right_action.activate.connect (() => {
            int page_num = notebook.page_num (tab);
            if (page_num >= 0 && page_num < notebook.get_n_pages () - 1) { notebook.reorder_child (tab, page_num + 1); save_settings (); }
        });
        tab_actions.add_action (right_action);

        var new_win_action = new GLib.SimpleAction ("move_new_window", null);
        new_win_action.activate.connect (() => {
            if (notebook.get_n_pages () <= 1) return;
            var app_ref = this.application as EditorApplication;
            var new_win = new EditorWindow (app_ref, false);
            var new_tab = new_win.get_current_tab ();
            if (tab.current_file != null) new_tab.current_file = tab.current_file;
            Gtk.TextIter start, end;
            tab.text_buffer.get_bounds (out start, out end);
            new_tab.text_buffer.set_text (tab.text_buffer.get_text (start, end, false), -1);
            new_win.update_title ();
            new_win.present ();
            notebook.remove_page (notebook.page_num (tab));
            save_settings ();
        });
        tab_actions.add_action (new_win_action);

        var close_others_action = new GLib.SimpleAction ("close_others", null);
        close_others_action.activate.connect (() => {
            for (int i = notebook.get_n_pages () - 1; i >= 0; i--) {
                if (notebook.get_nth_page (i) != tab) notebook.remove_page (i);
            }
            save_settings ();
        });
        tab_actions.add_action (close_others_action);

        var close_action = new GLib.SimpleAction ("close", null);
        close_action.activate.connect (() => { close_btn.clicked (); });
        tab_actions.add_action (close_action);

        var popover = new Gtk.PopoverMenu.from_model (tab_menu_model);
        popover.set_parent (tab_box);
        popover.has_arrow = false;
        popover.insert_action_group ("tab", tab_actions);

        var click_controller = new Gtk.GestureClick ();
        click_controller.button = Gdk.BUTTON_SECONDARY;
        click_controller.pressed.connect ((n_press, x, y) => {
            popover.set_pointing_to (Gdk.Rectangle () { x = (int)x, y = (int)y, width = 0, height = 0 });
            popover.popup ();
        });
        tab_box.add_controller (click_controller);
        
        notebook.append_page (tab, tab_box);
        notebook.set_tab_reorderable (tab, true);
        notebook.set_menu_label_text (tab, tab.get_tab_title ());
        notebook.set_current_page (notebook.get_n_pages () - 1);

        if (view_switcher != null) {
            view_switcher.set_stack (tab.view_stack);
        }
        
        tab.text_view.grab_focus ();
        return tab;
    }

    private void force_close_tab (EditorTab tab) {
        int page_num = notebook.page_num (tab);
        if (page_num >= 0) {
            notebook.remove_page (page_num);
            if (notebook.get_n_pages () == 0) create_new_tab ();
            save_settings ();
        }
    }

    private EditorTab? get_current_tab () {
        if (notebook == null) return null;
        int page_num = notebook.get_current_page ();
        if (page_num < 0) return null;
        return notebook.get_nth_page (page_num) as EditorTab;
    }

    private void update_search_count () {
        if (search_count_label == null) return;
        var tab = get_current_tab ();
        if (tab == null || search_entry.get_text ().length == 0) {
            search_count_label.label = "";
            return;
        }
        int count = tab.search_context.occurrences_count;
        if (count < 0) {
            search_count_label.label = "..."; // Still scanning in background
        } else if (count == 0) {
            search_count_label.label = _("No matches");
        } else {
            int pos = 0;
            Gtk.TextIter start, end;
            if (tab.text_buffer.get_selection_bounds (out start, out end)) {
                pos = tab.search_context.get_occurrence_position (start, end);
            }
            if (pos > 0) {
                search_count_label.label = _("%d of %d").printf (pos, count);
            } else {
                search_count_label.label = _("%d matches").printf (count);
            }
        }
    }

    private void update_zoom () {
        string css = ".editor-view { font-size: %dpt; }".printf (current_font_size);
        zoom_provider.load_from_string (css);
        if (status_zoom_button != null) {
            status_zoom_button.label = "Font Size: %dpt".printf (current_font_size);
        }
        save_settings ();
    }

    private FileState[] load_settings () {
        FileState[] files_to_open = new FileState[0];
        string config_dir = GLib.Path.build_filename (GLib.Environment.get_user_config_dir (), "json-lens");
        string path = GLib.Path.build_filename (config_dir, "settings.json");
        
        if (!GLib.FileUtils.test (path, GLib.FileTest.EXISTS)) {
            try {
                GLib.DirUtils.create_with_parents (config_dir, 0755);
                string default_settings = "{\n  \"font_size\": 12,\n  \"word_wrap\": true,\n  \"show_line_numbers\": true,\n  \"allow_comments\": false,\n  \"active_tab\": 0,\n  \"open_files\": []\n}";
                GLib.FileUtils.set_contents (path, default_settings);
            } catch (GLib.Error e) {
                // Fallback to defaults gracefully if we can't write to the config directory
            }
        }

        try {
            string contents;
            GLib.FileUtils.get_contents (path, out contents);
            
            Arena a = {}; a.init ();
            Arena scratch = {}; scratch.init ();
            JsonError err;
            JsonValue* root = json_parse (ref a, ref scratch, contents, contents.length, 1, out err);
            
            if (root != null) {
                current_font_size = (int) json_get_number (root, "font_size", 12.0);
                setting_word_wrap = json_get_bool (root, "word_wrap", true);
                setting_show_line_numbers = json_get_bool (root, "show_line_numbers", true);
                setting_allow_comments = json_get_bool (root, "allow_comments", false);
                active_tab_index = (int) json_get_number (root, "active_tab", 0.0);
                
                JsonValue* arr = json_get (root, "open_files");
                if (arr != null && arr->type == JsonType.ARRAY) {
                    for (size_t i = 0; i < arr->list_count; i++) {
                        JsonNode* item = arr->list_items + i;
                        if (item->value.type == JsonType.STRING) {
                            var fs = new FileState ();
                            fs.uri = item->value.string_val;
                            files_to_open += fs;
                        } else if (item->value.type == JsonType.OBJECT) {
                            var fs = new FileState ();
                            fs.uri = json_get_string (&item->value, "uri", "");
                            fs.view_name = json_get_string (&item->value, "view", "text");
                            fs.cursor_offset = (int) json_get_number (&item->value, "cursor", 0.0);
                            
                            JsonValue* path_arr = json_get (&item->value, "column_path");
                            if (path_arr != null && path_arr->type == JsonType.ARRAY) {
                                string[] temp_path = new string[0];
                                for (size_t j = 0; j < path_arr->list_count; j++) {
                                    JsonNode* p_item = path_arr->list_items + j;
                                    if (p_item->value.type == JsonType.STRING) {
                                        temp_path += p_item->value.string_val;
                                    }
                                }
                                fs.column_path = temp_path;
                            }
                            
                            if (fs.uri.length > 0) files_to_open += fs;
                        }
                    }
                }
            } else {
                current_font_size = 12;
            }
            
            scratch.free ();
            a.free ();
        } catch (GLib.Error e) {
            current_font_size = 12;
        }
        return files_to_open;
    }

    private void save_settings () {
        if (notebook == null) return;
        if (pending_loads > 0) return;
        
        string path = GLib.Path.build_filename (GLib.Environment.get_user_config_dir (), "json-lens", "settings.json");
        
        Arena a = {}; a.init ();
        Arena scratch = {}; scratch.init ();
        
        string contents = "";
        try { GLib.FileUtils.get_contents (path, out contents); } catch (GLib.Error e) {}
        
        JsonValue* root = null;
        if (contents.length > 0) {
            JsonError err;
            root = json_parse (ref a, ref scratch, contents, contents.length, 1, out err);
        }
        
        if (root == null || root->type != JsonType.OBJECT) {
            root = json_create_object (ref a);
        }
        
        json_replace_in_object (ref a, root, "font_size", json_create_number (ref a, current_font_size));
        json_replace_in_object (ref a, root, "word_wrap", json_create_bool (ref a, setting_word_wrap));
        json_replace_in_object (ref a, root, "show_line_numbers", json_create_bool (ref a, setting_show_line_numbers));
        json_replace_in_object (ref a, root, "allow_comments", json_create_bool (ref a, setting_allow_comments));
        json_replace_in_object (ref a, root, "active_tab", json_create_number (ref a, notebook.get_current_page ()));
        
        JsonValue* files_arr = json_create_array (ref a);
        for (int i = 0; i < notebook.get_n_pages (); i++) {
            var tab = notebook.get_nth_page (i) as EditorTab;
            if (tab != null && tab.current_file != null) {
                JsonValue* tab_obj = json_create_object (ref a);
                json_add (ref a, tab_obj, "uri", json_create_string (ref a, tab.current_file.get_uri ()));
                json_add (ref a, tab_obj, "view", json_create_string (ref a, tab.view_stack.visible_child_name));
                Gtk.TextIter iter;
                tab.text_buffer.get_iter_at_mark (out iter, tab.text_buffer.get_insert ());
                json_add (ref a, tab_obj, "cursor", json_create_number (ref a, iter.get_offset ()));
                
                JsonValue* path_arr = json_create_array (ref a);
                string[] current_path = tab.column_view.get_current_path ();
                foreach (string key in current_path) {
                    json_append (ref a, path_arr, json_create_string (ref a, key));
                }
                json_add (ref a, tab_obj, "column_path", path_arr);
                
                json_append (ref a, files_arr, tab_obj);
            }
        }
        json_replace_in_object (ref a, root, "open_files", files_arr);
        
        unowned string? json_str = json_to_string (ref a, root, true);
        if (json_str != null) {
            for (int i = 0; i < notebook.get_n_pages (); i++) {
                var tab = notebook.get_nth_page (i) as EditorTab;
                if (tab != null && tab.current_file != null && tab.current_file.get_path () == path) {
                    tab.is_saving = true;
                }
            }
            try {
                GLib.DirUtils.create_with_parents (GLib.Path.get_dirname (path), 0755);
                GLib.FileUtils.set_contents (path, json_str);
            } catch (GLib.Error e) {
            }
            GLib.Timeout.add (1000, () => {
                for (int i = 0; i < notebook.get_n_pages (); i++) {
                    var tab = notebook.get_nth_page (i) as EditorTab;
                    if (tab != null && tab.current_file != null && tab.current_file.get_path () == path) {
                        tab.is_saving = false;
                    }
                }
                return GLib.Source.REMOVE;
            });
        }
        scratch.free ();
        a.free ();
    }

    private void update_cursor_status () {
        if (status_cursor_label == null) return;
        var tab = get_current_tab ();
        if (tab == null || tab.text_buffer == null) {
            status_cursor_label.label = "Ln 1, Col 1";
            return;
        }
        Gtk.TextIter iter;
        tab.text_buffer.get_iter_at_mark (out iter, tab.text_buffer.get_insert ());
        int row = iter.get_line () + 1;
        int col = iter.get_line_offset () + 1;
        status_cursor_label.label = "Ln %d, Col %d".printf (row, col);
        
        update_search_count ();
    }

    private void update_title () {
        var tab = get_current_tab ();
        if (tab != null) {
            string file_name = tab.current_file != null ? tab.current_file.get_basename () : "Untitled";
            string prefix = tab.text_buffer.get_modified () ? "*" : "";
            title = "%s%s - JsonLens".printf (prefix, file_name);
            
            if (status_path_label != null) {
                if (tab.current_file != null) {
                    status_path_label.label = tab.current_file.get_path () ?? tab.current_file.get_uri ();
                } else {
                    status_path_label.label = "";
                }
            }
        } else {
            title = "Untitled - JsonLens";
            if (status_path_label != null) {
                status_path_label.label = "";
            }
        }
    }

    private void update_recent_menu () {
        recent_menu.remove_all ();
        var recent_manager = Gtk.RecentManager.get_default ();
        var items = recent_manager.get_items ();
        int count = 0;
        
        foreach (var item in items) {
            if (!item.has_application ("JsonLens")) continue;
            
            if (count >= 10) break;
            
            string uri = item.get_uri ();
            string display_name = item.get_display_name ();
            
            var menu_item = new GLib.MenuItem (display_name, "win.open_recent");
            menu_item.set_action_and_target_value ("win.open_recent", new GLib.Variant.string (uri));
            recent_menu.append_item (menu_item);
            count++;
        }
        
        if (count == 0) {
            var empty = new GLib.MenuItem (_("No Recent Files"), null);
            recent_menu.append_item (empty);
        }
    }

    private void on_open_button_clicked (GLib.SimpleAction action, GLib.Variant? parameter) {
        var dialog = new Gtk.FileDialog ();
        dialog.title = _("Open File");

        dialog.open.begin (this, null, (obj, res) => {
            try {
                var file_to_open = dialog.open.end (res);
                if (file_to_open != null) {
                    this.do_open_file_async.begin (file_to_open);
                }
            } catch (GLib.Error e) {
                // Ignored (e.g., user cancelled)
            }
        });
    }

    private void set_buffer_safe_mode (EditorTab tab, bool safe_mode) {
        if (safe_mode) {
            tab.text_view.wrap_mode = Gtk.WrapMode.CHAR; // PREVENTS GPU TEXTURE CRASH!
            tab.text_buffer.highlight_syntax = false;
            tab.text_buffer.highlight_matching_brackets = false;
            tab.search_context.highlight = false;
            tab.text_buffer.language = null; // PREVENTS REGEX ENGINE CRASH!
        } else {
            tab.text_view.wrap_mode = setting_word_wrap ? Gtk.WrapMode.WORD_CHAR : Gtk.WrapMode.NONE;
            tab.text_buffer.language = GtkSource.LanguageManager.get_default ().get_language ("json");
            tab.text_buffer.highlight_syntax = true;
            tab.text_buffer.highlight_matching_brackets = true;
            tab.search_context.highlight = true;
        }
    }

    private async void do_open_file_async (GLib.File file_to_open, EditorTab? target_tab = null, bool is_hot_reload = false, FileState? state = null) {
        pending_loads++;
        var tab = target_tab;
        if (tab == null) {
            tab = get_current_tab ();
            if (tab == null || tab.current_file != null || tab.text_buffer.get_char_count () > 0 || tab.is_loading) {
                tab = create_new_tab ();
            }
        }
        
        tab.is_loading = true;
        bool is_current = (tab == get_current_tab ());

        if (!is_hot_reload) {
            busy_spinner.start ();
            this.sensitive = false;
        }
        if (is_current) {
            status_left_label.label = is_hot_reload ? _("Reloading file...") : _("Opening file...");
        }

        try {
            uint8[] contents_bytes;
            string? etag_out;
            yield file_to_open.load_contents_async (null, out contents_bytes, out etag_out);

            var sb = new GLib.StringBuilder ();
            sb.append_len ((string) contents_bytes, contents_bytes.length);
            string contents_str = sb.str.make_valid ();
            
            var result = yield parse_json_in_background (contents_str, setting_allow_comments ? 1 : 0);
            
            string text_to_set = contents_str;
            
            Gtk.TextIter current_start, current_end;
            tab.text_buffer.get_bounds (out current_start, out current_end);
            string current_text = tab.text_buffer.get_text (current_start, current_end, false);
            
            if (current_text != text_to_set) {
                bool needs_safe = (!result.success && text_to_set.length > 50000);
                set_buffer_safe_mode (tab, needs_safe);
                tab.text_buffer.set_text (text_to_set, -1);
            }
            
            if (state != null && !is_hot_reload) {
                tab.column_view.pending_restore_path = state.column_path;
                Gtk.TextIter buf_start, buf_end;
                tab.text_buffer.get_bounds (out buf_start, out buf_end);
                string buffer_text = tab.text_buffer.get_text (buf_start, buf_end, false);
                tab.column_view.load_json (buffer_text);
                tab.view_stack.visible_child_name = state.view_name;
            }

            if (!result.success) {
                highlight_error (tab, result.err);
                if (is_current) {
                    status_left_label.label = _("Opened with errors at Ln %d, Col %d.").printf (result.err.line, result.err.col);
                }
            } else {
                if (!is_hot_reload) {
                    if (state != null) {
                        Gtk.TextIter iter;
                        tab.text_buffer.get_iter_at_offset (out iter, state.cursor_offset);
                        tab.text_buffer.place_cursor (iter);
                        GLib.Timeout.add (150, () => {
                            tab.text_view.scroll_to_mark (tab.text_buffer.get_insert (), 0.1, true, 0.5, 0.5);
                            return GLib.Source.REMOVE;
                        });
                    } else {
                        Gtk.TextIter start_iter;
                        tab.text_buffer.get_start_iter (out start_iter);
                        tab.text_buffer.place_cursor (start_iter);
                        GLib.Timeout.add (150, () => {
                            tab.text_view.scroll_to_mark (tab.text_buffer.get_insert (), 0.1, false, 0.0, 0.0);
                            return GLib.Source.REMOVE;
                        });
                    }
                }
                if (is_current) {
                    status_left_label.label = _("Opened %s").printf (file_to_open.get_basename ());
                }
            }
            tab.text_buffer.set_modified (false);

            Gtk.RecentManager.get_default ().add_item (file_to_open.get_uri ());
            update_recent_menu ();
            tab.current_file = file_to_open;
            tab.reset_monitor ();
            if (is_current) update_title ();
        } catch (GLib.Error e) {
            if (is_current) status_left_label.label = _("Error opening file");
            if (!is_hot_reload) {
                show_error_dialog (_("Error opening file: ") + e.message);
            }
            if (is_current) update_title();
        } finally {
            tab.is_loading = false;
            pending_loads--;
            if (pending_loads == 0 && !is_hot_reload) {
                GLib.Idle.add (() => {
                    save_settings ();
                    return GLib.Source.REMOVE;
                });
            }
            if (!is_hot_reload) {
                busy_spinner.stop ();
                this.sensitive = true;
                tab.text_view.grab_focus ();
            }
        }
    }

    private async void do_save_file_async (GLib.File file_to_save) {
        var tab = get_current_tab ();
        if (tab == null) return;

        tab.is_saving = true;
        busy_spinner.start();
        status_left_label.label = _("Saving file...");
        this.sensitive = false;
        try {
            Gtk.TextIter start, end;
            tab.text_buffer.get_bounds (out start, out end);
            string text = tab.text_buffer.get_text (start, end, false);

            var result = yield parse_json_in_background (text, setting_allow_comments ? 1 : 0);
            
            string final_save_text = text;
            if (!result.success) {
                highlight_error (tab, result.err);
            }
            
            uint8[] contents_bytes = final_save_text.data;

            string? old_etag = null;
            bool make_backup = false;
            string? actual_new_etag;
            bool success_status;

            success_status = yield file_to_save.replace_contents_async (
                contents_bytes,
                old_etag,
                make_backup,
                GLib.FileCreateFlags.REPLACE_DESTINATION,
                null, 
                out actual_new_etag
            );
            
            if (success_status) {
                tab.current_file = file_to_save;
                tab.hide_reload_banner ();
                tab.reset_monitor ();
                tab.text_buffer.set_modified (false);
                update_title ();
                Gtk.RecentManager.get_default ().add_item (file_to_save.get_uri ());
                update_recent_menu ();
                save_settings ();
                if (!result.success) {
                    status_left_label.label = _("Saved %s (Errors at Ln %d, Col %d)").printf (file_to_save.get_basename (), result.err.line, result.err.col);
                } else {
                    status_left_label.label = _("Saved %s").printf (file_to_save.get_basename ());
                }
            } else {
                status_left_label.label = _("Error saving file");
                show_error_dialog (_("Failed to save file (operation returned false)."));
            }

        } catch (GLib.Error e) {
            status_left_label.label = _("Error saving file");
            show_error_dialog (_("Error saving file: ") + e.message);
        } finally {
            busy_spinner.stop();
            this.sensitive = true;
            tab.text_view.grab_focus ();
            GLib.Timeout.add (1000, () => {
                tab.is_saving = false;
                return GLib.Source.REMOVE;
            });
        }
    }

    private void on_save_button_clicked (GLib.SimpleAction action, GLib.Variant? parameter) {
        var tab = get_current_tab ();
        if (tab == null) return;

        if (tab.current_file != null) {
            this.do_save_file_async.begin (tab.current_file);
        } else {
            this.activate_action(ACTION_SAVE_AS, null);
        }
    }

    private void on_save_as_button_clicked (GLib.SimpleAction action, GLib.Variant? parameter) {
        var tab = get_current_tab ();
        if (tab == null) return;

        var dialog = new Gtk.FileDialog ();
        dialog.title = _("Save File As");
        
        if (tab.current_file != null) {
            dialog.initial_file = tab.current_file;
        } else {
            dialog.initial_name = "Untitled.json";
        }

        dialog.save.begin (this, null, (obj, res) => {
            try {
                var file_to_save = dialog.save.end (res);
                if (file_to_save != null) {
                    this.do_save_file_async.begin (file_to_save);
                }
            } catch (GLib.Error e) {
                // Ignored (e.g., user cancelled)
            }
        });
    }

    private async void do_format_action (EditorTab tab, bool pretty) {
        busy_spinner.start ();
        this.sensitive = false;

        Gtk.TextIter start, end;
        tab.text_buffer.get_bounds (out start, out end);
        string text = tab.text_buffer.get_text (start, end, false);

        var result = yield format_json_in_background (text, pretty, setting_allow_comments ? 1 : 0);
        
        if (result.success && result.formatted_json != null) {
            string final_text = result.formatted_json;
            if (text != final_text) {
                Gtk.TextIter cursor_iter;
                tab.text_buffer.get_iter_at_mark (out cursor_iter, tab.text_buffer.get_insert ());
                int line = cursor_iter.get_line ();
                int col = cursor_iter.get_line_offset ();

                bool needs_safe = (!pretty && final_text.length > 50000);
                set_buffer_safe_mode (tab, needs_safe);
                tab.show_minify_banner (needs_safe);

                tab.text_buffer.set_text (final_text, -1);

                Gtk.TextIter new_cursor_iter;
                int max_lines = tab.text_buffer.get_line_count ();
                if (line >= max_lines) line = max_lines > 0 ? max_lines - 1 : 0;
                
                tab.text_buffer.get_iter_at_line (out new_cursor_iter, line);
                int chars_in_line = new_cursor_iter.get_chars_in_line ();
                new_cursor_iter.set_line_offset (col < chars_in_line ? col : chars_in_line);
                tab.text_buffer.place_cursor (new_cursor_iter);
                
                GLib.Timeout.add (100, () => {
                    tab.text_view.scroll_to_mark (tab.text_buffer.get_insert (), 0.1, true, 0.5, 0.5);
                    return GLib.Source.REMOVE;
                });
            }
        } else {
            highlight_error (tab, result.err);
        }

        busy_spinner.stop ();
        this.sensitive = true;
        tab.text_view.grab_focus ();
    }

    private void highlight_error (EditorTab tab, JsonError err) {
        Gtk.TextIter line_start, end, cursor_pos;
        int line = err.line > 0 ? err.line - 1 : 0;
        tab.text_buffer.get_iter_at_line (out line_start, line);
        
        cursor_pos = line_start;
        int col = err.col > 0 ? err.col - 1 : 0;
        cursor_pos.set_line_offset (col);
        tab.text_buffer.place_cursor (cursor_pos);

        end = line_start;
        end.forward_line (); // Expand to cover the entire line

        tab.text_buffer.apply_tag_by_name ("error", line_start, end);
        GLib.Timeout.add (150, () => {
            tab.text_view.scroll_to_mark (tab.text_buffer.get_insert (), 0.1, true, 0.5, 0.5);
            return GLib.Source.REMOVE;
        });
    }

    private void show_error_dialog (string message) {
        var dialog = new Gtk.AlertDialog ("%s", message);
        dialog.show (this);
    }

    private async ParseResult parse_json_in_background (string contents, int flags) {
        SourceFunc callback = parse_json_in_background.callback;
        var result = new ParseResult ();
        
        new GLib.Thread<void*> ("json-parser", () => {
            Arena main_arena = {};
            main_arena.init ();
            Arena scratch_arena = {};
            scratch_arena.init ();
            
            JsonValue* root_node = json_parse (ref main_arena, ref scratch_arena, contents, contents.length, flags, out result.err);
            scratch_arena.free ();
            
            if (root_node != null) {
                result.success = true;
                unowned string? pretty_json = json_to_string (ref main_arena, root_node, true);
                if (pretty_json != null) {
                    result.formatted_json = pretty_json.make_valid ();
                }
            } else {
                result.success = false;
            }
            
            main_arena.free ();
            
            GLib.Idle.add (() => {
                callback ();
                return GLib.Source.REMOVE;
            });
            return null;
        });
        
        yield;
        return result;
    }

    private async ParseResult format_json_in_background (string contents, bool pretty, int flags) {
        SourceFunc callback = format_json_in_background.callback;
        var result = new ParseResult ();
        
        new GLib.Thread<void*> ("json-saver", () => {
            Arena save_arena = {};
            save_arena.init ();
            Arena scratch_arena = {};
            scratch_arena.init ();
            
            JsonValue* new_root = json_parse (ref save_arena, ref scratch_arena, contents, contents.length, flags, out result.err);
            
            if (new_root != null) {
                result.success = true;
                unowned string? json_str = json_to_string (ref save_arena, new_root, pretty);
                if (json_str != null) {
                    string valid_str = json_str.make_valid ();
                    if (!pretty && valid_str.length > 50000) {
                        var sb = new GLib.StringBuilder ();
                        bool in_string = false;
                        bool escape = false;
                        for (int i = 0; i < valid_str.length; i++) {
                            char c = valid_str[i];
                            sb.append_c (c);
                            if (escape) {
                                escape = false;
                            } else if (c == '\\') {
                                escape = true;
                            } else if (c == '"') {
                                in_string = !in_string;
                            } else if (!in_string && (c == '{' || c == '[' || c == ',')) {
                                if (i + 1 < valid_str.length && valid_str[i+1] != '}' && valid_str[i+1] != ']') {
                                    sb.append_c ('\n');
                                }
                            }
                        }
                        result.formatted_json = sb.str;
                    } else {
                        result.formatted_json = valid_str;
                    }
                }
            } else {
                result.success = false;
            }
            
            scratch_arena.free ();
            save_arena.free ();
            
            GLib.Idle.add (() => {
                callback ();
                return GLib.Source.REMOVE;
            });
            return null;
        });
        
        yield;
        return result;
    }
}


public class EditorApplication : Gtk.Application {
    public EditorApplication () {
        GLib.Object (
            application_id: "com.example.JsonLens",
            flags: GLib.ApplicationFlags.DEFAULT_FLAGS
        );

        var quit_action = new GLib.SimpleAction ("quit", null);
        quit_action.activate.connect (this.on_quit_action);
        this.add_action (quit_action);
        this.set_accels_for_action(EditorWindow.ACTION_NEW, {"<Control>n", "<Control>t"});
        this.set_accels_for_action(EditorWindow.ACTION_QUIT, {"<Control>q"});
        this.set_accels_for_action(EditorWindow.ACTION_OPEN, {"<Control>o"});
        this.set_accels_for_action(EditorWindow.ACTION_SAVE, {"<Control>s"});
        this.set_accels_for_action(EditorWindow.ACTION_SAVE_AS, {"<Control><Shift>s"});
        this.set_accels_for_action("win.find", {"<Control>f"});
        this.set_accels_for_action("win.replace", {"<Control>h"});
        this.set_accels_for_action("win.undo", {"<Control>z"});
        this.set_accels_for_action("win.redo", {"<Control><Shift>z", "<Control>y"});
        this.set_accels_for_action("win.cut", {"<Control>x"});
        this.set_accels_for_action("win.copy", {"<Control>c"});
        this.set_accels_for_action("win.paste", {"<Control>v"});
        this.set_accels_for_action("win.select_all", {"<Control>a"});
        this.set_accels_for_action("win.format_json", {"<Alt><Shift>f"});
        this.set_accels_for_action("win.minify_json", {"<Control><Alt>m"});
        this.set_accels_for_action("win.toggle_fold", {"<Control>m"});
        this.set_accels_for_action(EditorWindow.ACTION_ZOOM_IN, {"<Control>plus", "<Control>equal"});
        this.set_accels_for_action(EditorWindow.ACTION_ZOOM_OUT, {"<Control>minus"});
        this.set_accels_for_action(EditorWindow.ACTION_ZOOM_RESET, {"<Control>0"});
        this.set_accels_for_action("win.tab_close", {"<Control>w"});
        this.set_accels_for_action("win.tab_move_left", {"<Control><Shift>Page_Up"});
        this.set_accels_for_action("win.tab_move_right", {"<Control><Shift>Page_Down"});
    }

    protected override void activate () {
        var window = new EditorWindow (this, true);
        window.present ();
    }

    private void on_quit_action (GLib.SimpleAction action, GLib.Variant? parameter) {
        this.quit();
    }

    public static int main (string[] args) {
        // For full i18n, you would initialize gettext here:
        // Intl.setlocale (LocaleCategory.ALL, "");
        // Intl.bindtextdomain (GETTEXT_PACKAGE, Environment.get_prgname() + "/../share/locale"); // Or your locale dir
        // Intl.bind_text_domain_codeset (GETTEXT_PACKAGE, "UTF-8");
        GtkSource.init ();
        var app = new EditorApplication ();
        return app.run (args);
    }
}