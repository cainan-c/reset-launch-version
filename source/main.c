// SPDX-License-Identifier: MIT
#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <zlib.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define PAGE_SIZE 64
#define MAX_RECORDS 8192
#define VISIBLE_ROWS 9
#define FB_WIDTH 1280
#define FB_HEIGHT 720
#define UI_BG RGBA8_MAXALPHA(12, 16, 24)
#define UI_WHITE RGBA8_MAXALPHA(232, 238, 246)
#define UI_MUTED RGBA8_MAXALPHA(135, 151, 170)
#define UI_CYAN RGBA8_MAXALPHA(92, 210, 226)
#define UI_GOLD RGBA8_MAXALPHA(255, 197, 92)
#define UI_GREEN RGBA8_MAXALPHA(117, 218, 159)

typedef struct {
    u64 application_id;
    u32 launch_version;
    bool launch_version_valid;
    char name[0x201];
} TitleEntry;

static TitleEntry *g_titles;
static size_t g_title_count;
static size_t g_selected;
static size_t g_first_visible;
static char g_status[192] = "Select a title.";
static NsApplicationControlData g_control_data;
static bool g_ncm_initialized;
static Framebuffer g_framebuffer;
static u32 g_fb_width;
static u32 g_fb_height;
static FT_Library g_ft;
static FT_Face g_faces[PlSharedFontType_Total];
static bool g_face_ready[PlSharedFontType_Total];
static const PlSharedFontType g_font_order[] = {
    PlSharedFontType_Standard, PlSharedFontType_NintendoExt,
    PlSharedFontType_ChineseSimplified, PlSharedFontType_ExtChineseSimplified,
    PlSharedFontType_ChineseTraditional, PlSharedFontType_KO
};

static void format_version(u32 version, char *output, size_t output_size) {
    const unsigned major = (version >> 26) & 0x3f;
    const unsigned minor = (version >> 20) & 0x3f;
    const unsigned micro = (version >> 16) & 0x0f;
    const unsigned revision = version & 0xffff;

    if (revision)
        snprintf(output, output_size, "%u.%u.%u+%u", major, minor, micro, revision);
    else
        snprintf(output, output_size, "%u.%u.%u", major, minor, micro);
}

static void format_title_id(u64 application_id, char *output, size_t output_size) {
    snprintf(output, output_size, "%016llX", (unsigned long long)application_id);
}

static bool copy_title_name(const char *source, char *output, size_t output_size) {
    size_t written = 0;
    for (size_t i = 0; source[i] != '\0' && written + 1 < output_size;) {
        unsigned char c = (unsigned char)source[i];
        size_t length = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
            (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
        if (length == 1 && (c < 0x20 || c == 0x7f)) {
            i++;
            continue;
        }
        if (length > 1) {
            bool valid = true;
            for (size_t j = 1; j < length; j++) {
                if (((unsigned char)source[i + j] & 0xc0) != 0x80) valid = false;
            }
            if (!valid || source[i + length - 1] == '\0') { i++; continue; }
        }
        if (written + length >= output_size) break;
        memcpy(output + written, source + i, length);
        written += length;
        i += length;
    }
    while (written && output[written - 1] == ' ') written--;
    output[written] = '\0';
    return written > 0;
}

static bool choose_nacp_name(NacpLanguageEntry *languages, size_t count,
    char *name, size_t name_size) {
    // Prefer the English slot when available; retain UTF-8 for CJK rendering.
    if (count > 0 && copy_title_name(languages[0].name, name, name_size)) return true;

    for (size_t i = 0; i < count; i++) {
        if (copy_title_name(languages[i].name, name, name_size)) return true;
    }
    return false;
}

static bool extract_nacp_name(NacpStruct *nacp, char *name, size_t name_size) {
    if (nacp->titles_data_format == 1) {
        const u16 compressed_size = nacp->lang_data.compressed_data.buffer_size;
        if (compressed_size == 0 ||
            compressed_size > sizeof(nacp->lang_data.compressed_data.buffer))
            return false;

        NacpLanguageEntry languages[32] = {0};
        z_stream stream = {0};
        stream.next_in = nacp->lang_data.compressed_data.buffer;
        stream.avail_in = compressed_size;
        stream.next_out = (Bytef *)languages;
        stream.avail_out = sizeof(languages);

        if (inflateInit2(&stream, -15) != Z_OK) return false;
        const int inflate_result = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        return inflate_result == Z_STREAM_END &&
            choose_nacp_name(languages, 32, name, name_size);
    }

    if (choose_nacp_name(nacp->lang_data.lang, 16, name, name_size)) return true;
    NacpLanguageEntry *language = NULL;
    return R_SUCCEEDED(nacpGetLanguageEntry(nacp, &language)) && language != NULL &&
        copy_title_name(language->name, name, name_size);
}

static bool extract_control_data_name(NsApplicationControlData *control_data,
    char *name, size_t name_size) {
    return extract_nacp_name(&control_data->nacp, name, name_size);
}

static bool read_title_name(TitleEntry *title, u8 acd_index) {
    u64 actual_size = 0;
    u32 unknown = 0;
    memset(&g_control_data, 0, sizeof(g_control_data));

    Result rc;
    if (acd_index == 0) {
        rc = nsGetApplicationControlData(NsApplicationControlSource_Storage,
            title->application_id, &g_control_data, sizeof(g_control_data), &actual_size);
    } else {
        // On HOS 20+, flag1 selects the Switch 2 Edition control-data variant.
        // flag2 is a separate field; passing 1 there still returns the S1 NACP.
        rc = nsGetApplicationControlData2(NsApplicationControlSource_Storage,
            title->application_id, &g_control_data, sizeof(g_control_data), 1, 0,
            &actual_size, &unknown);
    }
    return R_SUCCEEDED(rc) && extract_control_data_name(&g_control_data,
        title->name, sizeof(title->name));
}

static bool read_title_name_from_content(TitleEntry *title) {
    if (!g_ncm_initialized) {
        if (R_FAILED(ncmInitialize())) return false;
        g_ncm_initialized = true;
    }

    s32 total_statuses = 0;
    if (R_FAILED(nsCountApplicationContentMeta(title->application_id, &total_statuses)) ||
        total_statuses <= 0 || total_statuses > 256) return false;

    NsApplicationContentMetaStatus *statuses =
        calloc((size_t)total_statuses, sizeof(*statuses));
    if (statuses == NULL) return false;
    s32 status_count = 0;
    Result rc = nsListApplicationContentMetaStatus(title->application_id, 0,
        statuses, total_statuses, &status_count);
    if (R_FAILED(rc)) {
        free(statuses);
        return false;
    }

    bool found = false;
    for (s32 i = status_count - 1; i >= 0 && !found; i--) {
        const NcmStorageId storage_id = (NcmStorageId)statuses[i].storageID;
        if (storage_id != NcmStorageId_SdCard && storage_id != NcmStorageId_BuiltInUser &&
            storage_id != NcmStorageId_GameCard) continue;

        NcmContentMetaDatabase database = {0};
        NcmContentStorage storage = {0};
        FsFileSystem control_fs = {0};
        FsFile control_file = {0};
        NcmContentMetaKey key = {0};
        NcmContentId content_id = {0};
        u64 program_id = title->application_id;
        char content_path[0x301] = {0};
        bool file_open = false;
        bool fs_open = false;

        rc = ncmOpenContentMetaDatabase(&database, storage_id);
        if (R_FAILED(rc)) continue;
        rc = ncmContentMetaDatabaseGetLatestContentMetaKey(&database, &key,
            title->application_id);
        if (R_SUCCEEDED(rc))
            rc = ncmContentMetaDatabaseGetContentIdByType(&database, &content_id,
                &key, NcmContentType_Control);
        if (R_SUCCEEDED(rc)) rc = ncmOpenContentStorage(&storage, storage_id);
        if (R_SUCCEEDED(rc))
            rc = ncmContentStorageGetProgramId(&storage, &program_id, &content_id,
                FsContentAttributes_All);
        if (R_SUCCEEDED(rc))
            rc = ncmContentStorageGetPath(&storage, content_path, sizeof(content_path),
                &content_id);
        if (R_SUCCEEDED(rc))
            rc = fsOpenFileSystemWithId(&control_fs, program_id, FsFileSystemType_ContentControl,
                content_path, FsContentAttributes_All);
        if (R_SUCCEEDED(rc)) fs_open = true;
        if (R_SUCCEEDED(rc)) {
            const char *control_paths[2];
            if ((title->application_id >> 56) == 0x04) {
                control_paths[0] = "/Acd_1/control.nacp";
                control_paths[1] = "/control.nacp";
            } else {
                control_paths[0] = "/control.nacp";
                control_paths[1] = "/Acd_1/control.nacp";
            }
            for (size_t path_index = 0; path_index < sizeof(control_paths) / sizeof(control_paths[0]) && !file_open; path_index++) {
                rc = fsFsOpenFile(&control_fs, control_paths[path_index], FsOpenMode_Read, &control_file);
                if (R_SUCCEEDED(rc)) file_open = true;
            }
        }
        if (file_open) {
            u64 bytes_read = 0;
            memset(&g_control_data, 0, sizeof(g_control_data));
            rc = fsFileRead(&control_file, 0, &g_control_data.nacp,
                sizeof(g_control_data.nacp), 0, &bytes_read);
            if (R_SUCCEEDED(rc) && bytes_read >= sizeof(g_control_data.nacp))
                found = extract_nacp_name(&g_control_data.nacp,
                    title->name, sizeof(title->name));
        }

        if (file_open) fsFileClose(&control_file);
        if (fs_open) fsFsClose(&control_fs);
        ncmContentStorageClose(&storage);
        ncmContentMetaDatabaseClose(&database);
    }

    free(statuses);
    return found;
}

static bool title_name_is_id(const TitleEntry *title) {
    char title_id[17];
    format_title_id(title->application_id, title_id, sizeof(title_id));
    return strcmp(title->name, title_id) == 0;
}

static void load_title_name(TitleEntry *title) {
    const bool switch2_id = (title->application_id >> 56) == 0x04;
    if ((switch2_id && read_title_name(title, 1)) ||
        read_title_name(title, 0) || (!switch2_id && read_title_name(title, 1)) ||
        read_title_name_from_content(title)) return;
    // Keep a usable batch result if full control data is unavailable; a title
    // ID returned by NS is only a placeholder and must not suppress fallback.
    if (title->name[0] != '\0' && !title_name_is_id(title)) return;
    format_title_id(title->application_id, title->name, sizeof(title->name));
}

static void load_title_names_from_ns(TitleEntry *titles, s32 count) {
    if (count <= 0) return;

    u64 application_ids[PAGE_SIZE];
    for (s32 i = 0; i < count; i++) application_ids[i] = titles[i].application_id;

    size_t required_size = (size_t)count * sizeof(NacpLanguageEntry) +
        (size_t)count * sizeof(u64) + sizeof(NsApplicationControlData);
    size_t buffer_size = (required_size + 0xFFF) & ~(size_t)0xFFF;
    void *transfer_buffer = memalign(0x1000, buffer_size);
    if (transfer_buffer == NULL) return;
    memset(transfer_buffer, 0, buffer_size);

    AsyncValue async_value = {0};
    Result rc = nsListApplicationTitle2(&async_value, NsApplicationControlSource_Storage,
        application_ids, count, transfer_buffer, buffer_size);
    if (R_SUCCEEDED(rc)) {
        rc = asyncValueWait(&async_value, UINT64_MAX);
        u64 output_size = 0;
        s32 output_offset = -1;
        if (R_SUCCEEDED(rc)) rc = asyncValueGetSize(&async_value, &output_size);
        if (R_SUCCEEDED(rc)) rc = asyncValueGet(&async_value, &output_offset, sizeof(output_offset));
        if (R_SUCCEEDED(rc) && output_offset >= 0 &&
            (u64)output_offset <= buffer_size &&
            output_size <= buffer_size - (u64)output_offset) {
            const NacpLanguageEntry *entries =
                (const NacpLanguageEntry *)((const u8 *)transfer_buffer + output_offset);
            size_t entry_count = output_size / sizeof(*entries);
            if (entry_count > (size_t)count) entry_count = (size_t)count;
            for (size_t i = 0; i < entry_count; i++) {
                if (entries[i].name[0] != '\0')
                    copy_title_name(entries[i].name, titles[i].name, sizeof(titles[i].name));
            }
        }
        asyncValueClose(&async_value);
    }
    free(transfer_buffer);
}
static Result load_titles(void) {
    NsApplicationRecord page[PAGE_SIZE];
    s32 offset = 0;

    for (;;) {
        const size_t page_start = g_title_count;
        s32 count = 0;
        memset(page, 0, sizeof(page));
        Result rc = nsListApplicationRecord(page, PAGE_SIZE, offset, &count);
        if (R_FAILED(rc)) return rc;
        if (count < 0 || count > PAGE_SIZE) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        if (count == 0) break;
        if (g_title_count + (size_t)count > MAX_RECORDS)
            return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

        TitleEntry *grown = realloc(g_titles, (g_title_count + (size_t)count) * sizeof(*g_titles));
        if (grown == NULL) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        g_titles = grown;

        for (s32 i = 0; i < count; i++) {
            TitleEntry *title = &g_titles[g_title_count++];
            memset(title, 0, sizeof(*title));
            title->application_id = page[i].application_id;
            if (title->application_id == 0) {
                g_title_count--;
                continue;
            }
            title->launch_version_valid = R_SUCCEEDED(
                avmGetLaunchRequiredVersion(title->application_id, &title->launch_version));
        }

        const size_t page_title_count = g_title_count - page_start;
        if (page_title_count > 0) {
            load_title_names_from_ns(g_titles + page_start, (s32)page_title_count);
            for (size_t i = page_start; i < g_title_count; i++) {
                load_title_name(&g_titles[i]);
            }
        }

        offset += count;
        if (offset >= MAX_RECORDS || count < PAGE_SIZE) break;
    }

    return 0;
}

static Result reset_title(TitleEntry *title) {
    Result rc = avmPushLaunchVersion(title->application_id, 0);
    if (R_FAILED(rc)) return rc;

    Service manager = {0};
    rc = nsGetApplicationManagerInterface(&manager);
    if (R_FAILED(rc)) return rc;

    const struct {
        u32 version;
        u32 padding;
        u64 application_id;
        u8 trailing_padding[4];
    } input = {0, 0, title->application_id, {0}};
    rc = serviceDispatchIn(&manager, 36, input);
    serviceClose(&manager);
    if (R_FAILED(rc)) return rc;

    title->launch_version = 0;
    title->launch_version_valid = true;
    return 0;
}

static u32 decode_codepoint_utf8(const unsigned char *s, size_t *length) {
    if (s[0] < 0x80) { *length = 1; return s[0]; }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
        *length = 2; return ((s[0] & 0x1f) << 6) | (s[1] & 0x3f);
    }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        *length = 3; return ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
    }
    if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        *length = 4; return ((s[0] & 7) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
    }
    *length = 1;
    return 0xfffd;
}

static FT_Face find_face(u32 codepoint) {
    for (size_t i = 0; i < sizeof(g_font_order) / sizeof(g_font_order[0]); i++) {
        FT_Face face = g_faces[g_font_order[i]];
        if (g_face_ready[g_font_order[i]] && FT_Get_Char_Index(face, codepoint)) return face;
    }
    return g_face_ready[PlSharedFontType_Standard] ? g_faces[PlSharedFontType_Standard] : NULL;
}

static void draw_text(u32 *pixels, u32 x, u32 baseline, u32 color, const char *text, u32 max_width) {
    u32 pen_x = x;
    const unsigned char *p = (const unsigned char *)text;
    const u32 end_x = x + max_width;
    while (*p && pen_x < end_x) {
        size_t bytes = 1;
        u32 cp = decode_codepoint_utf8(p, &bytes);
        p += bytes;
        FT_Face face = find_face(cp);
        if (!face) continue;
        FT_UInt glyph = FT_Get_Char_Index(face, cp);
        if (!glyph) glyph = FT_Get_Char_Index(face, 0xfffd);
        if (FT_Load_Glyph(face, glyph, FT_LOAD_DEFAULT) || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) continue;
        FT_Bitmap *bm = &face->glyph->bitmap;
        const int left = (int)pen_x + face->glyph->bitmap_left;
        const int top = (int)baseline - face->glyph->bitmap_top;
        for (u32 gy = 0; gy < bm->rows; gy++) {
            const u8 *row = bm->pitch >= 0 ? bm->buffer + gy * bm->pitch : bm->buffer + (bm->rows - 1 - gy) * (u32)(-bm->pitch);
            int py = top + (int)gy;
            if (py < 0 || (u32)py >= g_fb_height) continue;
            for (u32 gx = 0; gx < bm->width; gx++) {
                int px = left + (int)gx;
                if (px < 0 || (u32)px >= g_fb_width || (u32)px >= end_x) continue;
                u8 alpha = row[gx];
                if (!alpha) continue;
                u32 *dst = &pixels[(u32)py * g_fb_width + (u32)px];
                const u32 inv = 255 - alpha;
                u32 r = (color & 0xff) * alpha / 255 + (*dst & 0xff) * inv / 255;
                u32 g = ((color >> 8) & 0xff) * alpha / 255 + ((*dst >> 8) & 0xff) * inv / 255;
                u32 b = ((color >> 16) & 0xff) * alpha / 255 + ((*dst >> 16) & 0xff) * inv / 255;
                *dst = RGBA8_MAXALPHA(r, g, b);
            }
        }
        pen_x += (u32)(face->glyph->advance.x >> 6);
    }
}

static void fill_rect(u32 *pixels, u32 x, u32 y, u32 width, u32 height, u32 color) {
    if (x >= g_fb_width || y >= g_fb_height) return;
    if (width > g_fb_width - x) width = g_fb_width - x;
    if (height > g_fb_height - y) height = g_fb_height - y;
    for (u32 row = y; row < y + height; row++)
        for (u32 col = x; col < x + width; col++) pixels[row * g_fb_width + col] = color;
}

static void draw_ui(void) {
    u32 stride = 0;
    u32 *pixels = (u32 *)framebufferBegin(&g_framebuffer, &stride);
    if (!pixels) return;
    g_fb_width = stride / sizeof(u32);
    g_fb_height = FB_HEIGHT;
    for (u32 y = 0; y < g_fb_height; y++)
        for (u32 x = 0; x < g_fb_width; x++) pixels[y * g_fb_width + x] = UI_BG;

    if (g_selected < g_first_visible) g_first_visible = g_selected;
    if (g_selected >= g_first_visible + VISIBLE_ROWS) g_first_visible = g_selected - VISIBLE_ROWS + 1;

    draw_text(pixels, 40, 54, UI_CYAN, "Reset Launch Version", 700);
    char subtitle[96];
    snprintf(subtitle, sizeof(subtitle), "%zu titles known to NS", g_title_count);
    draw_text(pixels, 40, 86, UI_MUTED, subtitle, 700);
    draw_text(pixels, 40, 132, UI_MUTED, "TITLE NAME", 580);
    draw_text(pixels, 680, 132, UI_MUTED, "TITLE ID", 280);
    draw_text(pixels, 1010, 132, UI_MUTED, "LAUNCH VERSION", 240);
    fill_rect(pixels, 40, 146, 1200, 2, RGBA8_MAXALPHA(51, 68, 84));

    for (size_t row = 0; row < VISIBLE_ROWS; row++) {
        const size_t index = g_first_visible + row;
        const u32 y = 160 + (u32)row * 46;
        if (index >= g_title_count) break;
        if (index == g_selected) fill_rect(pixels, 32, y - 5, 1210, 40, RGBA8_MAXALPHA(34, 48, 63));
        char title_id[17], version[32];
        format_title_id(g_titles[index].application_id, title_id, sizeof(title_id));
        if (g_titles[index].launch_version_valid) format_version(g_titles[index].launch_version, version, sizeof(version));
        else snprintf(version, sizeof(version), "unavailable");
        const u32 primary = index == g_selected ? UI_GOLD : UI_WHITE;
        draw_text(pixels, 42, y + 20, primary, g_titles[index].name, 610);
        draw_text(pixels, 680, y + 20, UI_MUTED, title_id, 280);
        draw_text(pixels, 1010, y + 20, g_titles[index].launch_version_valid ? UI_GREEN : UI_MUTED, version, 240);
    }

    draw_text(pixels, 40, 616, UI_WHITE, g_status, 1180);
    fill_rect(pixels, 32, 654, 1216, 2, RGBA8_MAXALPHA(91, 107, 124));
    draw_text(pixels, 40, 686, UI_CYAN, "D-pad: move    A: reset    B/Plus: exit", 1180);
    framebufferEnd(&g_framebuffer);
}

static Result initialize_renderer(void) {
    Result rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) return rc;
    if (FT_Init_FreeType(&g_ft)) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    for (s32 type = 0; type < PlSharedFontType_Total; type++) {
        PlFontData font;
        rc = plGetSharedFontByType(&font, (PlSharedFontType)type);
        if (R_FAILED(rc)) continue;
        if (FT_New_Memory_Face(g_ft, font.address, font.size, 0, &g_faces[type])) continue;
        if (FT_Set_Pixel_Sizes(g_faces[type], 0, 23)) { FT_Done_Face(g_faces[type]); continue; }
        g_face_ready[type] = true;
    }
    if (!g_face_ready[PlSharedFontType_Standard]) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    framebufferCreate(&g_framebuffer, nwindowGetDefault(), FB_WIDTH, FB_HEIGHT, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&g_framebuffer);
    return 0;
}

static void shutdown_renderer(void) {
    framebufferClose(&g_framebuffer);
    for (s32 i = 0; i < PlSharedFontType_Total; i++) if (g_face_ready[i]) FT_Done_Face(g_faces[i]);
    if (g_ft) FT_Done_FreeType(g_ft);
    plExit();
}

int main(void) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Result rc = nsInitialize();
    if (R_FAILED(rc)) {
        return 1;
    }

    rc = avmInitialize();
    if (R_FAILED(rc)) {
        nsExit();
        return 1;
    }

    rc = initialize_renderer();
    if (R_FAILED(rc)) { avmExit(); nsExit(); return 1; }

    rc = load_titles();
    if (R_FAILED(rc)) {
        snprintf(g_status, sizeof(g_status), "Could not list titles: 0x%08X", rc);
    }



    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 keys = padGetButtonsDown(&pad);

        if (keys & (HidNpadButton_B | HidNpadButton_Plus)) break;
        if (g_title_count > 0 && (keys & HidNpadButton_Up) && g_selected > 0) g_selected--;
        if (g_title_count > 0 && (keys & HidNpadButton_Down) && g_selected + 1 < g_title_count) g_selected++;

        if (g_title_count > 0 && (keys & HidNpadButton_A)) {
            if (!g_titles[g_selected].launch_version_valid) {
                snprintf(g_status, sizeof(g_status), "Launch version unavailable; reset skipped.");
            } else {
                char title_id[17];
                format_title_id(g_titles[g_selected].application_id, title_id, sizeof(title_id));
                rc = reset_title(&g_titles[g_selected]);
                if (R_SUCCEEDED(rc))
                    snprintf(g_status, sizeof(g_status), "Reset %.100s (%s) to 1.0.0.",
                        g_titles[g_selected].name, title_id);
                else
                    snprintf(g_status, sizeof(g_status), "Reset failed for %s: 0x%08X",
                        title_id, rc);
            }
        }

        draw_ui();
        svcSleepThread(16000000ULL);
    }

    if (g_ncm_initialized) ncmExit();
    avmExit();
    nsExit();
    shutdown_renderer();
    free(g_titles);
    return 0;
}
