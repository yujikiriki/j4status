/*
 * j4status - Status line generator
 *
 * Copyright © 2012-2013 Quentin "Sardem FF7" Glidic
 *
 * This file is part of j4status.
 *
 * j4status is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * j4status is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with j4status. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#ifdef HAVE_STRING_H
#include <string.h>
#endif /* HAVE_STRING_H */

#include <glib.h>
#include <glib/gprintf.h>

#include <j4status-plugin-output.h>

typedef struct {
    gchar start[25]; /* strlen("<span color='#FFFFFFFF'>") */
    gchar end[8];    /* strlen("</span>") */
} ColourStr;

#define COLOUR_STR(n) ColourStr n = { .start = "", .end = "</span>" };

struct _J4statusPluginContext {
    J4statusCoreInterface *core;
    gchar *label_separator;
    J4statusColour colours[_J4STATUS_STATE_SIZE];
    gboolean align;
    GByteArray *line;
};

struct _J4statusOutputPluginStream {
    J4statusPluginContext *context;
    J4statusCoreStream *stream;
    GDataInputStream *in;
    GOutputStream *out;
    GCancellable *cancellable;
};

static void
_j4status_pango_stream_read_callback(GObject *obj, GAsyncResult *res, gpointer user_data)
{
    J4statusOutputPluginStream *stream = user_data;
    GDataInputStream *in = G_DATA_INPUT_STREAM(obj);
    GError *error = NULL;

    gchar *line;
    line = g_data_input_stream_read_line_finish(in, res, NULL, &error);
    if ( line == NULL )
    {
        if ( error == NULL )
            return;

        if ( ! g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) )
        {
            g_warning("Input error: %s", error->message);
            j4status_core_stream_reconnect(stream->context->core, stream->stream);
        }
        g_clear_error(&error);
        return;
    }

    gchar *event_id = line;
    gchar *section_id = g_utf8_strchr(line, -1, ' ');
    if ( section_id != NULL )
    {
        *section_id++ = '\0';
        j4status_core_trigger_action(stream->context->core, section_id, event_id);
    }

    g_free(line);
    g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, stream->cancellable, _j4status_pango_stream_read_callback, stream);
}

static J4statusOutputPluginStream *
_j4status_pango_stream_new(J4statusPluginContext *context, J4statusCoreStream *core_stream)
{
    J4statusOutputPluginStream *stream;

    stream = g_slice_new0(J4statusOutputPluginStream);
    stream->context = context;
    stream->stream = core_stream;

    stream->out = j4status_core_stream_get_output_stream(stream->context->core, stream->stream);
    stream->in = g_data_input_stream_new(j4status_core_stream_get_input_stream(stream->context->core, stream->stream));

    stream->cancellable = g_cancellable_new();
    g_data_input_stream_read_line_async(stream->in, G_PRIORITY_DEFAULT, stream->cancellable, _j4status_pango_stream_read_callback, stream);

    return stream;
}

static void
_j4status_pango_stream_free(J4statusPluginContext *context, J4statusOutputPluginStream *stream)
{
    g_cancellable_cancel(stream->cancellable);
    g_object_unref(stream->cancellable);

    g_object_unref(stream->in);

    g_slice_free(J4statusOutputPluginStream, stream);
}

static gboolean
_j4status_pango_send_header(J4statusPluginContext *context, J4statusOutputPluginStream *stream, GError **error)
{
    static gchar *_j4status_pango_header = "";
    return g_output_stream_write_all(stream->out, _j4status_pango_header, 1, NULL, NULL, error);
}

static void
_j4status_pango_set_colour(ColourStr *out, J4statusColour colour)
{
    if ( colour.set )
        g_snprintf(out->start, 25, "<span color=\"%s\">", j4status_colour_to_hex(colour));
    else
        out->end[0] = '\0';
}

#define byte_append(b) G_STMT_START { guint8 b_ = (b); g_byte_array_append(context->line, &b_, 1); } G_STMT_END
static void
_j4status_pango_generate_line(J4statusPluginContext *context, GList *sections)
{
    g_byte_array_set_size(context->line, 0);
    GList *section_;
    J4statusSection *section;
    gboolean urgent = FALSE;
    for ( section_ = sections ; section_ != NULL ; section_ = g_list_next(section_) )
    {
        section = section_->data;
        const gchar *cache;
        if ( j4status_section_is_dirty(section) )
        {
            gchar *new_cache = NULL;
            const gchar *value;
            value = j4status_section_get_value(section);
            if ( value != NULL )
            {
                J4statusColour colour = {0};
                COLOUR_STR(colour_str);


                J4statusState state = j4status_section_get_state(section);
                urgent = urgent || ( state & J4STATUS_STATE_URGENT );
                colour = j4status_section_get_colour(section);
                if ( ! colour.set )
                    colour = context->colours[state & ~J4STATUS_STATE_FLAGS];
                _j4status_pango_set_colour(&colour_str, colour);

                gsize s = 1, l = 0, r = 0;

                if ( context->align )
                {
                    gint64 max_width;
                    max_width = j4status_section_get_max_width(section);

                    if ( max_width < 0 )
                    {
                        s = -max_width;
                        switch ( j4status_section_get_align(section) )
                        {
                        case J4STATUS_ALIGN_CENTER:
                            l = s / 2;
                            r = ( s + 1 ) / 2;
                        break;
                        case J4STATUS_ALIGN_LEFT:
                            r = s;
                        break;
                        case J4STATUS_ALIGN_RIGHT:
                            l = s;
                        break;
                        }
                    }
                }
                gchar align_left[s], align_right[s];
                memset(align_left, ' ', l); align_left[l] = '\0';
                memset(align_right, ' ', r); align_right[r] = '\0';

                const gchar *label;
                label = j4status_section_get_label(section);
                if ( label != NULL )
                {
                    COLOUR_STR(label_colour_str);
                    _j4status_pango_set_colour(&label_colour_str, j4status_section_get_label_colour(section));

                    new_cache = g_strdup_printf("%s%s%s%s%s%s%s%s%s", label_colour_str.start, label, label_colour_str.end, context->label_separator, align_left, colour_str.start, value, colour_str.end, align_right);
                }
                else
                    new_cache = g_strdup_printf("%s%s%s%s%s", align_left, colour_str.start, value, colour_str.end, align_right);
            }
            j4status_section_set_cache(section, new_cache);
            cache = new_cache;
        }
        else
            cache = j4status_section_get_cache(section);
        if ( cache == NULL )
            continue;

        guint64 length, size;
        length = strlen(cache);
        size = GUINT64_TO_BE(length);
        byte_append('s');
        g_byte_array_append(context->line, (const guint8 *) &size, sizeof(size));
        g_byte_array_append(context->line, (const guint8 *) cache, length);

    }
    if ( urgent )
        byte_append('u');
    byte_append('\0');
}

static gboolean
_j4status_pango_send_line(J4statusPluginContext *context, J4statusOutputPluginStream *stream, GError **error)
{
    return g_output_stream_write_all(stream->out, context->line->data, context->line->len, NULL, NULL, error);
}

static void
_j4status_pango_update_colour(J4statusColour *colour, GKeyFile *key_file, gchar *name)
{
    gchar *config;
    config = g_key_file_get_string(key_file, "Pango", name, NULL);
    if ( config == NULL )
        return;

    *colour = j4status_colour_parse(config);
    g_free(config);
}

static J4statusPluginContext *
_j4status_pango_init(J4statusCoreInterface *core)
{
    J4statusPluginContext *context;

    context = g_new0(J4statusPluginContext, 1);
    context->core = core;

    context->colours[J4STATUS_STATE_UNAVAILABLE].set = TRUE;
    context->colours[J4STATUS_STATE_UNAVAILABLE].blue = 0xff;
    context->colours[J4STATUS_STATE_BAD].set = TRUE;
    context->colours[J4STATUS_STATE_BAD].red = 0xff;
    context->colours[J4STATUS_STATE_AVERAGE].set = TRUE;
    context->colours[J4STATUS_STATE_AVERAGE].red = 0xff;
    context->colours[J4STATUS_STATE_AVERAGE].green = 0xff;
    context->colours[J4STATUS_STATE_GOOD].set = TRUE;
    context->colours[J4STATUS_STATE_GOOD].green = 0xff;

    GKeyFile *key_file;
    key_file = j4status_config_get_key_file("Pango");

    if ( key_file != NULL )
    {
        context->align = g_key_file_get_boolean(key_file, "Pango", "Align", NULL);
        context->label_separator = g_key_file_get_string(key_file, "Pango", "LabelSeparator", NULL);
        _j4status_pango_update_colour(&context->colours[J4STATUS_STATE_NO_STATE], key_file, "NoStateColour");
        _j4status_pango_update_colour(&context->colours[J4STATUS_STATE_UNAVAILABLE], key_file, "UnavailableColour");
        _j4status_pango_update_colour(&context->colours[J4STATUS_STATE_BAD], key_file, "BadColour");
        _j4status_pango_update_colour(&context->colours[J4STATUS_STATE_AVERAGE], key_file, "AverageColour");
        _j4status_pango_update_colour(&context->colours[J4STATUS_STATE_GOOD], key_file, "GoodColour");
    }

    if ( context->label_separator == NULL )
        context->label_separator = g_strdup(": ");

    if ( key_file != NULL )
        g_key_file_free(key_file);

    context->line = g_byte_array_new();

    return context;
}

static void
_j4status_pango_uninit(J4statusPluginContext *context)
{
    g_byte_array_unref(context->line);

    g_free(context->label_separator);

    g_free(context);
}

void
j4status_output_plugin(J4statusOutputPluginInterface *interface)
{
    libj4status_output_plugin_interface_add_init_callback(interface, _j4status_pango_init);
    libj4status_output_plugin_interface_add_uninit_callback(interface, _j4status_pango_uninit);

    libj4status_output_plugin_interface_add_stream_new_callback(interface, _j4status_pango_stream_new);
    libj4status_output_plugin_interface_add_stream_free_callback(interface, _j4status_pango_stream_free);

    libj4status_output_plugin_interface_add_send_header_callback(interface, _j4status_pango_send_header);
    libj4status_output_plugin_interface_add_generate_line_callback(interface, _j4status_pango_generate_line);
    libj4status_output_plugin_interface_add_send_line_callback(interface, _j4status_pango_send_line);
}
