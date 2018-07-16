/*
 * @file flv-parser.c
 * @author Akagi201
 * @date 2015/02/04
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <inttypes.h>

#include "flv-parser.h"

// File-scope ("global") variables
const char *flv_signature = "FLV";

enum scriptdata_value_types {
    SCRIPTDATA_NUMBER,
    SCRIPTDATA_BOOLEAN,
    SCRIPTDATA_STRING,
    SCRIPTDATA_OBJECT,
    SCRIPTDATA_MOVIE_CLIP,
    SCRIPTDATA_NULL,
    SCRIPTDATA_UNDEFINED,
    SCRIPTDATA_REFERENCE,
    SCRIPTDATA_ECMA_ARRAY,
    SCRIPTDATA_OBJECT_END_MARKER,
    SCRIPTDATA_STRICT_ARRAY,
    SCRIPTDATA_DATE,
    SCRIPTDATA_LONG_STRING,
};
const char *scriptdata_value_type_names[] = {
    "Number",       // DOUBLE
    "Boolean",      // UI8
    "String",       // SCRIPTDATASTRING:      {Length UI16, Data STRING(no terminating NUL)}
    "Object",       // SCRIPTDATAOBJECT       {Properties, List Terminator}
    "MovieClip",    // (reserved, not supported)
    "Null",
    "Undefined",
    "Reference",    // UI16
    "ECMA array",   // SCRIPTDATAECMAARRAY    {Length UI32, Variables, List Terminator}
    "Object end marker",
    "Strict array", // SCRIPTDATASTRICTARRAY: {Length UI32, Value SCRIPTDATAVALUE[Length]}
    "Date",         // SCRIPTDATADATE:        {DateTime DOUBLE, LocalDateTimeOffset SI16}
    "Long string",  // SCRIPTDATALONGSTRING:  {Length UI32, Data STRING(no terminating NUL)}
};

const char *sound_formats[] = {
        "Linear PCM, platform endian",
        "ADPCM",
        "MP3",
        "Linear PCM, little endian",
        "Nellymoser 16-kHz mono",
        "Nellymoser 8-kHz mono",
        "Nellymoser",
        "G.711 A-law logarithmic PCM",
        "G.711 mu-law logarithmic PCM",
        "not defined by standard",
        "AAC",
        "Speex",
        "not defined by standard",
        "not defined by standard",
        "MP3 8-Khz",
        "Device-specific sound"
};

const char *sound_rates[] = {
        "5.5-Khz",
        "11-Khz",
        "22-Khz",
        "44-Khz"
};

const char *sound_sizes[] = {
        "8 bit",
        "16 bit"
};

const char *sound_types[] = {
        "Mono",
        "Stereo"
};

const char *frame_types[] = {
        "not defined by standard",
        "keyframe (for AVC, a seekable frame)",
        "inter frame (for AVC, a non-seekable frame)",
        "disposable inter frame (H.263 only)",
        "generated keyframe (reserved for server use only)",
        "video info/command frame"
};

const char *codec_ids[] = {
        "not defined by standard",
        "JPEG (currently unused)",
        "Sorenson H.263",
        "Screen video",
        "On2 VP6",
        "On2 VP6 with alpha channel",
        "Screen video version 2",
        "AVC"
};

const char *avc_packet_types[] = {
        "AVC sequence header",
        "AVC NALU",
        "AVC end of sequence (lower level NALU sequence ender is not required or supported)"
};

static FILE *g_infile;
static int g_v_count;
static int g_a_count;

void die(void) {
    if (g_infile) {
        fpos_t pos;
        fgetpos(g_infile, &pos);
        printf("Error at %lld!\n", pos);
    } else {
        printf("Error!\n");
    }

    exit(-1);
}

/*
 * @brief read bits from 1 byte
 * @param[in] value: 1 byte to analysize
 * @param[in] start_bit: start from the low bit side
 * @param[in] count: number of bits
 */
uint8_t flv_get_bits(uint8_t value, uint8_t start_bit, uint8_t count) {
    uint8_t mask = 0;

    mask = (uint8_t) (((1 << count) - 1) << start_bit);
    return (mask & value) >> start_bit;

}

void flv_print_header(flv_header_t *flv_header) {

    printf("FLV file version %u\n", flv_header->version);
    printf("  Contains audio tags: ");
    if (flv_header->type_flags & (1 << FLV_HEADER_AUDIO_BIT)) {
        printf("Yes\n");
    } else {
        printf("No\n");
    }
    printf("  Contains video tags: ");
    if (flv_header->type_flags & (1 << FLV_HEADER_VIDEO_BIT)) {
        printf("Yes\n");
    } else {
        printf("No\n");
    }
    printf("  Data offset: %lu\n", (unsigned long) flv_header->data_offset);

    return;
}

size_t fread_1(uint8_t *ptr) {
    assert(NULL != ptr);
    return fread(ptr, 1, 1, g_infile);
}

size_t fread_3(uint32_t *ptr) {
    assert(NULL != ptr);
    size_t count = 0;
    uint8_t bytes[3] = {0};
    *ptr = 0;
    count = fread(bytes, 3, 1, g_infile);
    *ptr = (bytes[0] << 16) | (bytes[1] << 8) | bytes[2];
    return count * 3;
}

size_t fread_4(uint32_t *ptr) {
    assert(NULL != ptr);
    size_t count = 0;
    uint8_t bytes[4] = {0};
    *ptr = 0;
    count = fread(bytes, 4, 1, g_infile);
    *ptr = (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
    return count * 4;
}

/*
 * @brief skip 4 bytes in the file stream
 */
size_t fread_4s(uint32_t *ptr) {
    assert(NULL != ptr);
    size_t count = 0;
    uint8_t bytes[4] = {0};
    *ptr = 0;
    count = fread(bytes, 4, 1, g_infile);
    return count * 4;
}

/*
 * @brief read scriptdata tag
 */
double hex2double(uint8_t *hex) {
    uint32_t bin_h = (hex[0] << 24) | (hex[1] << 16) | (hex[2] << 8) | hex[3];
    uint32_t bin_l = (hex[4] << 24) | (hex[5] << 16) | (hex[6] << 8) | hex[7];
    uint64_t bin = ((uint64_t)bin_h << 32) | (uint64_t)bin_l;

    union {
        uint64_t i;
        double d;
    } cnv;
    cnv.i = bin;

    return cnv.d; // *((double *)&bin);
}
double parse_scriptdata_number(uint8_t **data, size_t *len) {
    if (!data || !*data || !len) {
        return 0;
    }

    uint8_t *d = *data;
    size_t l = *len;

    if (l < 1) {
        return 0;
    }
    uint8_t type = *d;
    assert(SCRIPTDATA_NUMBER == type);
    d += 1;
    l -= 1;

    if (l < 8) {
        return 0;
    }
    double num = hex2double(d);
    d += 8;
    l -= 8;

    *data = d;
    *len = l;
    return num;
}
uint8_t parse_scriptdata_boolean(uint8_t **data, size_t *len) {
    if (!data || !*data || !len) {
        return 0;
    }

    uint8_t *d = *data;
    size_t l = *len;

    if (l < 1) {
        return 0;
    }
    uint8_t type = *d;
    assert(SCRIPTDATA_BOOLEAN == type);
    d += 1;
    l -= 1;

    if (l < 1) {
        return 0;
    }
    uint8_t b = d[0];
    d += 1;
    l -= 1;

    *data = d;
    *len = l;
    return b;
}
char *parse_scriptdata_string_without_type(uint8_t **data, size_t *len) {
    if (!data || !*data || !len) {
        return NULL;
    }

    uint8_t *d = *data;
    size_t l = *len;

    if (l < 2) {
        return NULL;
    }
    uint16_t length = (d[0] << 8) + d[1];
    d += 2;
    l -= 2;

    // string: pascal to c
    if (l < length) {
        return NULL;
    }
    uint8_t *src = d;
    uint8_t *dst = src - 1;
    if (length) {
        memmove(dst, src, length);
    }
    dst[length] = '\0';
    d += length;
    l -= length;

    *data = d;
    *len = l;
    return (char *) dst;
}
char *parse_scriptdata_string(uint8_t **data, size_t *len) {
    if (!data || !*data || !len) {
        return NULL;
    }

    uint8_t *d = *data;
    size_t l = *len;

    if (l < 1) {
        return NULL;
    }
    uint8_t type = *d;
    assert(SCRIPTDATA_STRING == type);
    d += 1;
    l -= 1;

    *data = d;
    *len = l;
    return parse_scriptdata_string_without_type(data, len);
}
uint8_t *parse_scriptdata_ECMA_array_raw(uint8_t **data, size_t *len, uint8_t *type, uint32_t *length) {
    if (!data || !*data || !len || !type || !length) {
        return NULL;
    }

    uint8_t *d = *data;
    size_t l = *len;

    if (l < 1) {
        return NULL;
    }
    *type = *d;
    assert(SCRIPTDATA_ECMA_ARRAY == *type);
    d += 1;
    l -= 1;

    if (l < 4) {
        return NULL;
    }
    *length = (d[0] << 24) + (d[1] << 16) + (d[2] << 8) + d[3];
    d += 4;
    l -= 4;

    *data = d;
    *len = l;
    return d;
}
uint32_t parse_scriptdata_strict_array(uint8_t **data, size_t *len) {
    if (!data || !*data || !len) {
        return 0;
    }

    uint8_t *d = *data;
    size_t l = *len;

    if (l < 1) {
        return 0;
    }
    uint8_t type = *d;
    assert(SCRIPTDATA_STRICT_ARRAY == type);
    d += 1;
    l -= 1;

    if (l < 4) {
        return 0;
    }
    uint32_t length = (d[0] << 24) + (d[1] << 16) + (d[2] << 8) + d[3];
    d += 4;
    l -= 4;

    // items
    for (uint32_t i = 0; i < length; i++) {
        // TODO: support NUMBER only
        parse_scriptdata_number(&d, &l);
    }

    *data = d;
    *len = l;
    return length;
}
double parse_scriptdata_date(uint8_t **data, size_t *len) {
    if (!data || !*data || !len) {
        return 0;
    }

    uint8_t *d = *data;
    size_t l = *len;

    if (l < 1) {
        return 0;
    }
    uint8_t type = *d;
    assert(SCRIPTDATA_DATE == type);
    d += 1;
    l -= 1;

    if (l < 8) {
        return 0;
    }
    double date_time_ms = hex2double(d);
    d += 8;
    l -= 8;

    if (l < 2) {
        return 0;
    }
    int16_t offset_min = (d[0] << 8) | d[1];
    d += 2;
    l -= 2;

    // date_time_ms: Number of milliseconds since Jan 1, 1970 UTC
    // offset_min: Local time offset in minutes from UTC
    (void)offset_min;

    *data = d;
    *len = l;
    return date_time_ms;
}
void print_scriptdata_object(uint8_t **ppvalue_data, size_t *pvalue_length) {
    if (!ppvalue_data || !*ppvalue_data || !pvalue_length) {
        return;
    }

    while (*pvalue_length > 0) {
        char *property_name = parse_scriptdata_string_without_type(ppvalue_data, pvalue_length);
        if (property_name == NULL) {
            break;
        }
        if (*pvalue_length < 1) {
            break;
        }
        uint8_t property_type = (*ppvalue_data)[0];

        // check terminator
        if (property_name[0] == '\0' && property_type == SCRIPTDATA_OBJECT_END_MARKER) {
            printf("      Property: %s\n", scriptdata_value_type_names[property_type]);
            *ppvalue_data += 1;
            *pvalue_length -= 1;
            break;
        }

        switch (property_type) {
            case SCRIPTDATA_NUMBER:
                printf("      Property: %s %s %f\n",
                    property_name,
                    scriptdata_value_type_names[property_type],
                    parse_scriptdata_number(ppvalue_data, pvalue_length));
                break;
            case SCRIPTDATA_BOOLEAN:
                printf("      Property: %s %s %u\n",
                    property_name,
                    scriptdata_value_type_names[property_type],
                    parse_scriptdata_boolean(ppvalue_data, pvalue_length));
                break;
            case SCRIPTDATA_STRING:
                printf("      Property: %s %s %s\n",
                    property_name,
                    scriptdata_value_type_names[property_type],
                    parse_scriptdata_string(ppvalue_data, pvalue_length));
                break;
            case SCRIPTDATA_OBJECT:
                printf("      Property: %s %s\n",
                    property_name,
                    scriptdata_value_type_names[property_type]);
                *ppvalue_data += 1;
                *pvalue_length -= 1;
                printf("        ---- begin Object ----\n");
                print_scriptdata_object(ppvalue_data, pvalue_length);
                printf("        ---- end Object ----\n");
                break;
            case SCRIPTDATA_STRICT_ARRAY:
                printf("      property: %s %s %u[items]\n",
                    property_name,
                    scriptdata_value_type_names[property_type],
                    parse_scriptdata_strict_array(ppvalue_data, pvalue_length));
                break;
            case SCRIPTDATA_DATE:
                {
                double date_time_ms = parse_scriptdata_date(ppvalue_data, pvalue_length);
                /*
                struct timespec ts;
                ts.tv_sec = date_time_ms / 1000.0;
                ts.tv_nsec = (date_time_ms - ts.tv_sec * 1000) * 1000 * 1000;
                */
                struct tm local_time = {0};
                time_t date_time_sec = date_time_ms / 1000.0;
                localtime_r(&date_time_sec, &local_time);

                char date[256];
                strftime(date, sizeof(date), "%F %T %z (%Z)", &local_time);

                printf("      property: %s %s %f[msec] %ld[sec] %s\n",
                    property_name,
                    scriptdata_value_type_names[property_type],
                    date_time_ms,
                    date_time_sec,
                    date
                    );
                }
                break;
            default:
                printf("      Unknown property: %s %u %s\n",
                    property_name,
                    property_type,
                    scriptdata_value_type_names[property_type]);
                return;
        }
    }
}
scriptdata_tag_t *read_scriptdata_tag(flv_tag_t *flv_tag) {
    assert(NULL != flv_tag);
    if (flv_tag->data_size <= 0) {
        return NULL;
    }

    scriptdata_tag_t *tag = malloc(sizeof(scriptdata_tag_t));
    tag->data = malloc((size_t) flv_tag->data_size);
    fread(tag->data, 1, (size_t) flv_tag->data_size, g_infile);

    uint8_t *data = tag->data;
    size_t data_size = flv_tag->data_size;

    // Name
    char *name_str = parse_scriptdata_string(&data, &data_size);
    if (name_str == NULL) {
        return tag;
    }

    // Value
    uint8_t value_type;
    uint32_t value_num_of_items;
    uint8_t *value_data = parse_scriptdata_ECMA_array_raw(&data, &data_size, &value_type, &value_num_of_items);
    if (value_data == NULL) {
        return tag;
    }
    size_t value_length = (tag->data + flv_tag->data_size) - value_data;

    printf("  Scriptdata tag:\n");
    printf("    Name:  %s\n", name_str);
    printf("    Value: %s (%u items, %zu bytes)\n", scriptdata_value_type_names[value_type], value_num_of_items, value_length);
    print_scriptdata_object(&value_data, &value_length);
    assert(value_length == 0);

    return tag;
}

/*
 * @brief read audio tag
 */
audio_tag_t *read_audio_tag(flv_tag_t *flv_tag) {
    assert(NULL != flv_tag);
    size_t count = 0;
    uint8_t byte = 0;
    audio_tag_t *tag = NULL;

    if (flv_tag->data_size == 0) {
        return NULL;
    }

    tag = malloc(sizeof(audio_tag_t));
    count = fread_1(&byte);

    tag->sound_format = flv_get_bits(byte, 4, 4);
    tag->sound_rate = flv_get_bits(byte, 2, 2);
    tag->sound_size = flv_get_bits(byte, 1, 1);
    tag->sound_type = flv_get_bits(byte, 0, 1);

    printf("  Audio tag:\n");
    printf("    Sound format: %u - %s\n", tag->sound_format, sound_formats[tag->sound_format]);
    printf("    Sound rate: %u - %s\n", tag->sound_rate, sound_rates[tag->sound_rate]);

    printf("    Sound size: %u - %s\n", tag->sound_size, sound_sizes[tag->sound_size]);
    printf("    Sound type: %u - %s\n", tag->sound_type, sound_types[tag->sound_type]);

    uint8_t aac_packet_type = 255; // dummy
    if (tag->sound_format == 10) {
        // AACPacketType
        count += fread_1(&byte);
        printf("      AAC packet type: %u - %s\n", byte, (byte == 0) ? "AAC sequence header" : "AAC raw");
        aac_packet_type = byte;
    }
    tag->data = malloc((size_t) flv_tag->data_size - count);
    count = fread(tag->data, 1, (size_t) flv_tag->data_size - count, g_infile);

    if (aac_packet_type == 0 && count > 0) {
        // The AudioSpecificConfig is defined in ISO 14496-3.
        // Note that this is not the same as the contents of the esds box from an MP4/F4V file.
        uint8_t *p = tag->data;
        uint8_t *p_end = p + count;

        printf("      AAC AudioSpecificConfig:");
        while (p < p_end) {
            printf(" 0x%x", *p++);
        }
        printf("\n");
    }

    return tag;
}

/*
 * @brief read video tag
 */
video_tag_t *read_video_tag(flv_tag_t *flv_tag) {
    size_t count = 0;
    uint8_t byte = 0;
    video_tag_t *tag = NULL;

    if (flv_tag->data_size == 0) {
        return NULL;
    }

    tag = malloc(sizeof(video_tag_t));

    count = fread_1(&byte);

    tag->frame_type = flv_get_bits(byte, 4, 4);
    tag->codec_id = flv_get_bits(byte, 0, 4);

    printf("  Video tag:\n");
    printf("    Frame type: %u - %s\n", tag->frame_type, frame_types[tag->frame_type]);
    printf("    Codec ID: %u - %s\n", tag->codec_id, codec_ids[tag->codec_id]);

    // AVC-specific stuff
    if (tag->codec_id == FLV_CODEC_ID_AVC) {
        tag->data = read_avc_video_tag(tag, flv_tag, (uint32_t) (flv_tag->data_size - count));
    } else {
        tag->data = malloc((size_t) flv_tag->data_size - count);
        count = fread(tag->data, 1, (size_t) flv_tag->data_size - count, g_infile);
    }

    return tag;
}

/*
 * @brief read AVC video tag
 */
avc_video_tag_t *read_avc_video_tag(video_tag_t *video_tag, flv_tag_t *flv_tag, uint32_t data_size) {
    size_t count = 0;
    avc_video_tag_t *tag = NULL;

    tag = malloc(sizeof(avc_video_tag_t));

    count = fread_1(&(tag->avc_packet_type));
    if (tag->avc_packet_type == 1) {
        //count += fread_4s(&(tag->composition_time));
        count += fread_3(&(tag->composition_time));
    } else {
        tag->composition_time = 0;
    }

    assert(video_tag->frame_type != 5); // not implemented!

    // AVCVIDEOPACKET
    size_t data_len = data_size - count;
    if (tag->avc_packet_type == 0) {
        // AVCDecoderConfigurationRecord
        tag->nalu_len = 0;
    } else if (tag->avc_packet_type == 1) {
        // One or more NALUs (Full frames are required)
        count += fread_4(&(tag->nalu_len));
    } else {
        // do nothing
        tag->nalu_len = 0;
    }

    printf("    AVC video tag:\n");
    printf("      AVC packet type: %u - %s\n", tag->avc_packet_type, avc_packet_types[tag->avc_packet_type]);
    printf("      AVC composition time: %i\n", tag->composition_time);
    printf("      AVC 1st nalu length: %i\n", tag->nalu_len);
    printf("      AVC packet data length: %lu\n", data_len);

    tag->data = malloc((size_t) data_size - count);
    count = fread(tag->data, 1, (size_t) data_size - count, g_infile);

    return tag;
}

void flv_parser_init(FILE *in_file) {
    g_infile = in_file;
}

int flv_parser_run() {
    flv_tag_t *tag;
    flv_read_header();

    for (; ;) {
        tag = flv_read_tag(); // read the tag
        if (!tag) {
            return 0;
        }
        flv_free_tag(tag); // and free it
    }

}

void flv_free_tag(flv_tag_t *tag) {
    scriptdata_tag_t *scriptdata_tag;
    audio_tag_t *audio_tag;
    video_tag_t *video_tag;
    avc_video_tag_t *avc_video_tag;

    if (tag->data) {
        if (tag->tag_type == TAGTYPE_SCRIPTDATAOBJECT) {
            scriptdata_tag = (scriptdata_tag_t *) tag->data;
            free(scriptdata_tag->data);
        } else if (tag->tag_type == TAGTYPE_VIDEODATA) {
            video_tag = (video_tag_t *) tag->data;
            if (video_tag->codec_id == FLV_CODEC_ID_AVC) {
                avc_video_tag = (avc_video_tag_t *) video_tag->data;
                free(avc_video_tag->data);
            }
            free(video_tag->data);
        } else if (tag->tag_type == TAGTYPE_AUDIODATA) {
            audio_tag = (audio_tag_t *) tag->data;
            free(audio_tag->data);
        }
    }

    free(tag->data);
    free(tag);
}

int flv_read_header(void) {
    size_t count = 0;
    int i = 0;
    flv_header_t *flv_header = NULL;

    flv_header = malloc(sizeof(flv_header_t));
    count = fread(flv_header, 1, sizeof(flv_header_t), g_infile);

    // XXX strncmp
    for (i = 0; i < strlen(flv_signature); i++) {
        assert(flv_header->signature[i] == flv_signature[i]);
    }

    flv_header->data_offset = ntohl(flv_header->data_offset);

    flv_print_header(flv_header);

    return 0;

}

void print_general_tag_info(flv_tag_t *tag) {
    assert(NULL != tag);
    printf("  Data size: %lu\n", (unsigned long) tag->data_size);
    printf("  Timestamp: %lu\n", (unsigned long) tag->timestamp);
    printf("  Timestamp extended: %u\n", tag->timestamp_ext);
    printf("  StreamID: %lu\n", (unsigned long) tag->stream_id);

    return;
}

flv_tag_t *flv_read_tag(void) {
    size_t count = 0;
    uint32_t prev_tag_size = 0;
    flv_tag_t *tag = NULL;

    count = fread_4(&prev_tag_size);
    printf("Prev tag size: %lu\n", (unsigned long) prev_tag_size);
    printf("\n");

    // Start reading next tag
    tag = malloc(sizeof(flv_tag_t));
    count = fread_1(&(tag->tag_type));
    if (feof(g_infile)) {
        free(tag);
        return NULL;
    }
    count = fread_3(&(tag->data_size));
    count = fread_3(&(tag->timestamp));
    count = fread_1(&(tag->timestamp_ext));
    count = fread_3(&(tag->stream_id));

    printf("Tag type: %u - ", tag->tag_type);
    switch (tag->tag_type) {
        case TAGTYPE_AUDIODATA:
            printf("Audio data #%d\n", g_a_count++);
            print_general_tag_info(tag);
            tag->data = (void *) read_audio_tag(tag);
            break;
        case TAGTYPE_VIDEODATA:
            printf("Video data #%d\n", g_v_count++);
            print_general_tag_info(tag);
            tag->data = (void *) read_video_tag(tag);
            break;
        case TAGTYPE_SCRIPTDATAOBJECT:
            printf("Script data object\n");
            print_general_tag_info(tag);
            tag->data = (void *) read_scriptdata_tag(tag);
            break;
        default:
            printf("Unknown tag type!\n");
            free(tag);
            die();
    }

    #if 0
    // Did we reach end of file?
    if (feof(g_infile)) {
        return NULL;
    }
    #endif

    return tag;
}
