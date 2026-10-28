// _nanotts.c

#define Py_LIMITED_API 0x03090000
#include <Python.h>

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <picoapi.h>
#include <picoapid.h>
#include <picoos.h>

#define PICO_MEM_SIZE (2500000u)
#define MAX_OUTBUF_SIZE (128u)

// If pico_Int16 is 16-bit signed (typical), this cap avoids truncation.
#ifndef PICO_INT16_MAX
#define PICO_INT16_MAX 32767
#endif

// Upper bound for markup level values; keeps the int conversion defined and
// the formatted tags within their fixed-size buffers.
#define MAX_MARKUP_LEVEL 10000

static const char *pico_supported_lang[] = {"en-US", "en-GB", "de-DE",
                                            "es-ES", "fr-FR", "it-IT"};
static const char *pico_internal_ta[] = {"en-US_ta.bin", "en-GB_ta.bin",
                                         "de-DE_ta.bin", "es-ES_ta.bin",
                                         "fr-FR_ta.bin", "it-IT_ta.bin"};
static const char *pico_internal_sg[] = {
    "en-US_lh0_sg.bin", "en-GB_kh0_sg.bin", "de-DE_gl0_sg.bin",
    "es-ES_zl0_sg.bin", "fr-FR_nk0_sg.bin", "it-IT_cm0_sg.bin"};
static const int pico_num_supported = 6;

// Pico voice name. Pico APIs take pico_Char*. On typical Pico builds pico_Char
// == char. If your Pico build defines pico_Char differently, you must provide a
// real conversion.
static const pico_Char pico_voice_name[] = "PicoVoice";

// Synthesis runs without the GIL, so errors are collected here and raised as
// Python exceptions once the GIL is held again.
typedef enum {
    SYNTH_OK = 0,
    SYNTH_ERR_MEMORY,
    SYNTH_ERR_VALUE,
    SYNTH_ERR_RUNTIME,
} synth_error_kind;

typedef struct {
    synth_error_kind kind;
    char message[256];
} synth_error;

static int set_error(synth_error *err, synth_error_kind kind, const char *fmt,
                     ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, ap);
    va_end(ap);
    err->kind = kind;
    return 0;
}

static void raise_synth_error(const synth_error *err) {
    switch (err->kind) {
    case SYNTH_ERR_MEMORY:
        PyErr_NoMemory();
        break;
    case SYNTH_ERR_VALUE:
        PyErr_SetString(PyExc_ValueError, err->message);
        break;
    default:
        PyErr_SetString(PyExc_RuntimeError, err->message);
        break;
    }
}

static int find_voice_index(const char *voice) {
    for (int i = 0; i < pico_num_supported; i++) {
        if (strcmp(pico_supported_lang[i], voice) == 0) {
            return i;
        }
    }
    return -1;
}

// Convert Python unicode to a new bytes object (UTF-8 encoded) and return both
// the owning bytes object and borrowed c-string pointer valid while bytes
// lives.
static int unicode_to_utf8_bytes(PyObject *obj, PyObject **out_bytes,
                                 const char **out_cstr) {
    *out_bytes = NULL;
    *out_cstr = NULL;

    PyObject *bytes = PyUnicode_AsUTF8String(obj);
    if (!bytes) {
        return 0;
    }
    const char *cstr = PyBytes_AsString(bytes);
    if (!cstr) {
        Py_DECREF(bytes);
        return 0;
    }
    *out_bytes = bytes;
    *out_cstr = cstr;
    return 1;
}

static int append_pcm(unsigned char **buffer, size_t *size, size_t *capacity,
                      const unsigned char *data, size_t data_len) {
    if (data_len == 0)
        return 1;

    // Overflow check: needed = *size + data_len
    if (data_len > (SIZE_MAX - *size)) {
        return 0;
    }
    size_t needed = *size + data_len;

    if (needed > *capacity) {
        size_t new_cap = (*capacity == 0) ? 4096u : *capacity;

        // Grow while checking overflow
        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2) {
                new_cap = needed; // last resort; realloc to exact needed
                break;
            }
            new_cap *= 2;
        }

        unsigned char *new_buf = (unsigned char *)realloc(*buffer, new_cap);
        if (!new_buf) {
            return 0;
        }
        *buffer = new_buf;
        *capacity = new_cap;
    }

    memcpy(*buffer + *size, data, data_len);
    *size += data_len;
    return 1;
}

static int markup_level(double value) {
    double level = ceil(value * 100.0);
    if (!(level >= 0.0)) { // also catches NaN
        return 0;
    }
    if (level > MAX_MARKUP_LEVEL) {
        return MAX_MARKUP_LEVEL;
    }
    return (int)level;
}

// Wrap text in speed/pitch/volume markup (negative values are omitted).
//
// Pico interprets markup anywhere in its input, including <genfile> and <play>
// which write and read arbitrary files. There is no escape syntax, so every
// '<' in the caller's text is replaced with a space. Only an ASCII '<' starts
// a tag, and it can't appear inside a multi-byte UTF-8 sequence.
//
// Returns a malloc'd string, or NULL with err set.
static char *build_synth_text(const char *text_utf8, double speed, double pitch,
                              double volume, synth_error *err) {
    // Each tag is at most 22 chars with level <= MAX_MARKUP_LEVEL
    char open_tags[96];
    char close_tags[32];
    size_t open_len = 0;

    open_tags[0] = '\0';
    if (speed >= 0.0) {
        open_len += (size_t)snprintf(open_tags + open_len,
                                     sizeof(open_tags) - open_len,
                                     "<speed level=\"%d\">", markup_level(speed));
    }
    if (pitch >= 0.0) {
        open_len += (size_t)snprintf(open_tags + open_len,
                                     sizeof(open_tags) - open_len,
                                     "<pitch level=\"%d\">", markup_level(pitch));
    }
    if (volume >= 0.0) {
        open_len += (size_t)snprintf(
            open_tags + open_len, sizeof(open_tags) - open_len,
            "<volume level=\"%d\">", markup_level(volume));
    }

    snprintf(close_tags, sizeof(close_tags), "%s%s%s",
             (volume >= 0.0) ? "</volume>" : "",
             (pitch >= 0.0) ? "</pitch>" : "",
             (speed >= 0.0) ? "</speed>" : "");
    const size_t close_len = strlen(close_tags);

    const size_t text_len = strlen(text_utf8);
    if (text_len > SIZE_MAX - open_len - close_len - 1) {
        set_error(err, SYNTH_ERR_MEMORY, "out of memory");
        return NULL;
    }

    char *synth_text = (char *)malloc(open_len + text_len + close_len + 1);
    if (!synth_text) {
        set_error(err, SYNTH_ERR_MEMORY, "out of memory");
        return NULL;
    }

    memcpy(synth_text, open_tags, open_len);
    char *dst = synth_text + open_len;
    for (size_t i = 0; i < text_len; i++) {
        dst[i] = (text_utf8[i] == '<') ? ' ' : text_utf8[i];
    }
    memcpy(dst + text_len, close_tags, close_len);
    synth_text[open_len + text_len + close_len] = '\0';

    return synth_text;
}

// Safe helper: allocate path buffer for lingware file paths
static pico_Char *alloc_path_buf(void) {
    size_t cap =
        (size_t)PICO_MAX_DATAPATH_NAME_SIZE + (size_t)PICO_MAX_FILE_NAME_SIZE;
    pico_Char *buf = (pico_Char *)malloc(cap);
    return buf;
}

static int format_path(pico_Char *dst, const char *dir, const char *file) {
    size_t cap =
        (size_t)PICO_MAX_DATAPATH_NAME_SIZE + (size_t)PICO_MAX_FILE_NAME_SIZE;
    // snprintf expects char*. pico_Char is typically char in Pico builds.
    // If pico_Char differs, this needs a real conversion.
    int n = snprintf((char *)dst, cap, "%s/%s", dir, file);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return 1;
}

static int set_pico_error(synth_error *err, pico_System pico_system,
                          pico_Status status, const char *what) {
    pico_Retstring out_msg;
    if (pico_system) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        return set_error(err, SYNTH_ERR_RUNTIME, "%s failed: %s", what,
                         out_msg);
    }
    return set_error(err, SYNTH_ERR_RUNTIME, "%s failed with status %d", what,
                     (int)status);
}

// Drain available PCM from the engine into the output buffer.
static int drain_pcm(pico_System pico_system, pico_Engine pico_engine,
                     unsigned char **pcm_data, size_t *pcm_size,
                     size_t *pcm_capacity, synth_error *err) {
    unsigned char outbuf[MAX_OUTBUF_SIZE];
    pico_Int16 bytes_recv = 0;
    pico_Int16 out_data_type = 0;
    pico_Int32 get_status = 0;

    do {
        bytes_recv = 0;
        get_status = pico_getData(pico_engine, (void *)outbuf,
                                  (pico_Int16)MAX_OUTBUF_SIZE, &bytes_recv,
                                  &out_data_type);

        if (get_status != PICO_STEP_BUSY && get_status != PICO_STEP_IDLE) {
            return set_pico_error(err, pico_system, get_status,
                                  "pico_getData");
        }

        if (bytes_recv < 0 || bytes_recv > (pico_Int16)MAX_OUTBUF_SIZE) {
            return set_error(err, SYNTH_ERR_RUNTIME,
                             "pico_getData returned invalid bytes_recv");
        }

        if (bytes_recv > 0) {
            if (!append_pcm(pcm_data, pcm_size, pcm_capacity, outbuf,
                            (size_t)bytes_recv)) {
                return set_error(err, SYNTH_ERR_MEMORY, "out of memory");
            }
        }
    } while (get_status == PICO_STEP_BUSY);

    return 1;
}

// Feed UTF-8 text to Pico in safe chunks, gather PCM bytes in a growing buffer.
static int synthesize_to_pcm(pico_System pico_system, pico_Engine pico_engine,
                             const char *synth_text, unsigned char **out_pcm,
                             size_t *out_pcm_size, synth_error *err) {
    *out_pcm = NULL;
    *out_pcm_size = 0;

    unsigned char *pcm_data = NULL;
    size_t pcm_size = 0;
    size_t pcm_capacity = 0;

    const unsigned char *inp = (const unsigned char *)synth_text;

    // IMPORTANT: include trailing NUL as end-of-input marker for Pico
    const size_t inp_len = strlen((const char *)inp) + 1;
    size_t offset = 0;

    while (offset < inp_len) {
        size_t remaining = inp_len - offset;

        pico_Int16 chunk = (remaining > (size_t)PICO_INT16_MAX)
                               ? (pico_Int16)PICO_INT16_MAX
                               : (pico_Int16)remaining;

        pico_Int16 bytes_sent = 0;
        pico_Status status = pico_putTextUtf8(
            pico_engine, (const pico_Char *)(inp + offset), chunk, &bytes_sent);
        if (status != PICO_OK) {
            set_pico_error(err, pico_system, status, "pico_putTextUtf8");
            goto fail;
        }

        // bytes_sent should always be positive and <= chunk
        if (bytes_sent <= 0 || bytes_sent > chunk) {
            set_error(err, SYNTH_ERR_RUNTIME,
                      "pico_putTextUtf8 returned invalid bytes_sent");
            goto fail;
        }

        offset += (size_t)bytes_sent;

        if (!drain_pcm(pico_system, pico_engine, &pcm_data, &pcm_size,
                       &pcm_capacity, err)) {
            goto fail;
        }
    }

    // Final drain (often still needed)
    if (!drain_pcm(pico_system, pico_engine, &pcm_data, &pcm_size,
                   &pcm_capacity, err)) {
        goto fail;
    }

    *out_pcm = pcm_data;
    *out_pcm_size = pcm_size;
    return 1;

fail:
    free(pcm_data);
    return 0;
}

// Full synthesis without touching the Python API, so it can run with the GIL
// released. Each call gets its own Pico system, so calls may run concurrently.
static int synthesize(const char *text_utf8, int lang_index,
                      const char *lingware_path, double speed, double pitch,
                      double volume, unsigned char **out_pcm,
                      size_t *out_pcm_size, synth_error *err) {
    int ok = 0;
    pico_Status status;

    void *pico_mem = NULL;
    pico_System pico_system = NULL;
    pico_Resource pico_ta = NULL;
    pico_Resource pico_sg = NULL;
    pico_Engine pico_engine = NULL;
    int voice_created = 0;

    pico_Char *ta_file = NULL;
    pico_Char *sg_file = NULL;
    pico_Char *ta_res_name = NULL;
    pico_Char *sg_res_name = NULL;

    char *synth_text =
        build_synth_text(text_utf8, speed, pitch, volume, err);
    if (!synth_text) {
        return 0;
    }

    // Zeroed because Pico reads some of its pool before writing it, which
    // otherwise makes the output vary between calls.
    pico_mem = calloc(1, (size_t)PICO_MEM_SIZE);
    ta_file = alloc_path_buf();
    sg_file = alloc_path_buf();
    ta_res_name = (pico_Char *)malloc((size_t)PICO_MAX_RESOURCE_NAME_SIZE);
    sg_res_name = (pico_Char *)malloc((size_t)PICO_MAX_RESOURCE_NAME_SIZE);
    if (!pico_mem || !ta_file || !sg_file || !ta_res_name || !sg_res_name) {
        set_error(err, SYNTH_ERR_MEMORY, "out of memory");
        goto cleanup;
    }

    if (!format_path(ta_file, lingware_path, pico_internal_ta[lang_index]) ||
        !format_path(sg_file, lingware_path, pico_internal_sg[lang_index])) {
        set_error(err, SYNTH_ERR_VALUE, "lingware_path too long");
        goto cleanup;
    }

    status = pico_initialize(pico_mem, (pico_Int32)PICO_MEM_SIZE, &pico_system);
    if (status != PICO_OK) {
        set_pico_error(err, pico_system, status, "pico_initialize");
        goto cleanup;
    }

    status = pico_loadResource(pico_system, ta_file, &pico_ta);
    if (status != PICO_OK) {
        set_pico_error(err, pico_system, status, "pico_loadResource ta");
        goto cleanup;
    }

    status = pico_loadResource(pico_system, sg_file, &pico_sg);
    if (status != PICO_OK) {
        set_pico_error(err, pico_system, status, "pico_loadResource sg");
        goto cleanup;
    }

    status = pico_getResourceName(pico_system, pico_ta, ta_res_name);
    if (status != PICO_OK) {
        set_pico_error(err, pico_system, status, "pico_getResourceName ta");
        goto cleanup;
    }

    status = pico_getResourceName(pico_system, pico_sg, sg_res_name);
    if (status != PICO_OK) {
        set_pico_error(err, pico_system, status, "pico_getResourceName sg");
        goto cleanup;
    }

    status = pico_createVoiceDefinition(pico_system, pico_voice_name);
    if (status != PICO_OK) {
        set_pico_error(err, pico_system, status, "pico_createVoiceDefinition");
        goto cleanup;
    }
    voice_created = 1;

    status = pico_addResourceToVoiceDefinition(pico_system, pico_voice_name,
                                               ta_res_name);
    if (status != PICO_OK) {
        set_pico_error(err, pico_system, status,
                       "pico_addResourceToVoiceDefinition ta");
        goto cleanup;
    }

    status = pico_addResourceToVoiceDefinition(pico_system, pico_voice_name,
                                               sg_res_name);
    if (status != PICO_OK) {
        set_pico_error(err, pico_system, status,
                       "pico_addResourceToVoiceDefinition sg");
        goto cleanup;
    }

    status = pico_newEngine(pico_system, pico_voice_name, &pico_engine);
    if (status != PICO_OK) {
        set_pico_error(err, pico_system, status, "pico_newEngine");
        goto cleanup;
    }

    ok = synthesize_to_pcm(pico_system, pico_engine, synth_text, out_pcm,
                           out_pcm_size, err);

cleanup:
    if (pico_engine)
        pico_disposeEngine(pico_system, &pico_engine);
    if (voice_created)
        pico_releaseVoiceDefinition(pico_system, (pico_Char *)pico_voice_name);
    if (pico_ta)
        pico_unloadResource(pico_system, &pico_ta);
    if (pico_sg)
        pico_unloadResource(pico_system, &pico_sg);
    if (pico_system)
        pico_terminate(&pico_system);

    free(ta_file);
    free(sg_file);
    free(ta_res_name);
    free(sg_res_name);
    free(pico_mem);
    free(synth_text);

    return ok;
}

static PyObject *nanotts_speak(PyObject *self, PyObject *args,
                               PyObject *kwargs) {
    (void)self;

    static char *kwlist[] = {"text",   "voice",         "speed", "pitch",
                             "volume", "lingware_path", NULL};

    PyObject *text_obj = NULL;
    PyObject *voice_obj = Py_None;
    PyObject *speed_obj = Py_None;
    PyObject *pitch_obj = Py_None;
    PyObject *volume_obj = Py_None;
    PyObject *lingware_obj = Py_None;

    // Defaults
    const char *voice_cstr = "en-GB";
    const char *lingware_path = "./lang";
    const char *text_utf8 = NULL;

    PyObject *voice_bytes = NULL;
    PyObject *lingware_bytes = NULL;
    PyObject *text_bytes = NULL;
    PyObject *result = NULL;

    double speed = -1.0;
    double pitch = -1.0;
    double volume = -1.0;

    // Parse: required unicode text + optional objects
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "U|OOOOO", kwlist, &text_obj,
                                     &voice_obj, &speed_obj, &pitch_obj,
                                     &volume_obj, &lingware_obj)) {
        return NULL;
    }

    if (voice_obj != Py_None) {
        if (!PyUnicode_Check(voice_obj)) {
            PyErr_SetString(PyExc_TypeError, "voice must be a string or None");
            goto done;
        }
        if (!unicode_to_utf8_bytes(voice_obj, &voice_bytes, &voice_cstr)) {
            goto done;
        }
    }

    if (lingware_obj != Py_None) {
        if (!PyUnicode_Check(lingware_obj)) {
            PyErr_SetString(PyExc_TypeError,
                            "lingware_path must be a string or None");
            goto done;
        }
        if (!unicode_to_utf8_bytes(lingware_obj, &lingware_bytes,
                                   &lingware_path)) {
            goto done;
        }
    }

    if (speed_obj != Py_None) {
        speed = PyFloat_AsDouble(speed_obj);
        if (PyErr_Occurred())
            goto done;
    }
    if (pitch_obj != Py_None) {
        pitch = PyFloat_AsDouble(pitch_obj);
        if (PyErr_Occurred())
            goto done;
    }
    if (volume_obj != Py_None) {
        volume = PyFloat_AsDouble(volume_obj);
        if (PyErr_Occurred())
            goto done;
    }

    const int lang_index = find_voice_index(voice_cstr);
    if (lang_index < 0) {
        PyErr_SetString(PyExc_ValueError, "Unsupported voice");
        goto done;
    }

    if (!unicode_to_utf8_bytes(text_obj, &text_bytes, &text_utf8)) {
        goto done;
    }

    // The C strings stay valid without the GIL: they point into bytes objects
    // we hold references to, and bytes are immutable.
    unsigned char *pcm_data = NULL;
    size_t pcm_size = 0;
    synth_error err = {SYNTH_OK, ""};
    int ok;

    Py_BEGIN_ALLOW_THREADS
    ok = synthesize(text_utf8, lang_index, lingware_path, speed, pitch, volume,
                    &pcm_data, &pcm_size, &err);
    Py_END_ALLOW_THREADS

    if (ok) {
        result = PyBytes_FromStringAndSize((const char *)pcm_data,
                                           (Py_ssize_t)pcm_size);
    } else {
        raise_synth_error(&err);
    }
    free(pcm_data);

done:
    Py_XDECREF(text_bytes);
    Py_XDECREF(voice_bytes);
    Py_XDECREF(lingware_bytes);
    return result;
}

static PyMethodDef NanottsMethods[] = {{"speak", (PyCFunction)nanotts_speak,
                                        METH_VARARGS | METH_KEYWORDS,
                                        "Synthesize speech to PCM bytes."},
                                       {NULL, NULL, 0, NULL}};

static struct PyModuleDef nanotts_module = {PyModuleDef_HEAD_INIT, "_nanotts",
                                            "NanoTTS C extension", -1,
                                            NanottsMethods};

PyMODINIT_FUNC PyInit__nanotts(void) {
    return PyModule_Create(&nanotts_module);
}
