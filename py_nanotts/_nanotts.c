// _nanotts.c

#define Py_LIMITED_API 0x03090000
#include <Python.h>

#include <limits.h>
#include <math.h>
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

// Safe helper: join "<tag ...>" prefixes and "</tag>" suffixes around text
// Returns synth_text pointer and sets *need_free if malloc'd.
// On failure, sets Python exception and returns NULL.
static char *build_synth_text(const char *text_utf8, double speed, double pitch,
                              double volume, int *need_free) {
    *need_free = 0;

    char open_tags[128];
    char close_tags[128];
    open_tags[0] = '\0';
    close_tags[0] = '\0';

    // Helper macro to prepend to close_tags safely.
    // We build close_tags by prepending the new close tag before existing
    // close_tags.
#define PREPEND_CLOSE_TAG(CLOSE_STR)                                           \
    do {                                                                       \
        char _tmp[128];                                                        \
        int _n =                                                               \
            snprintf(_tmp, sizeof(_tmp), "%s%s", (CLOSE_STR), close_tags);     \
        if (_n < 0 || (size_t)_n >= sizeof(_tmp)) {                            \
            PyErr_SetString(PyExc_ValueError, "SSML tags too long");           \
            return NULL;                                                       \
        }                                                                      \
        memcpy(close_tags, _tmp, (size_t)_n + 1);                              \
    } while (0)

    // Helper to append to open_tags safely.
#define APPEND_OPEN_TAG(FMT, LEVEL)                                            \
    do {                                                                       \
        char _buf[64];                                                         \
        int _n = snprintf(_buf, sizeof(_buf), (FMT), (LEVEL));                 \
        if (_n < 0 || (size_t)_n >= sizeof(_buf)) {                            \
            PyErr_SetString(PyExc_ValueError, "SSML tag formatting failed");   \
            return NULL;                                                       \
        }                                                                      \
        size_t _cur = strlen(open_tags);                                       \
        if (_cur + (size_t)_n >= sizeof(open_tags)) {                          \
            PyErr_SetString(PyExc_ValueError, "SSML tags too long");           \
            return NULL;                                                       \
        }                                                                      \
        memcpy(open_tags + _cur, _buf, (size_t)_n + 1);                        \
    } while (0)

    if (speed >= 0.0) {
        int level = (int)ceil(speed * 100.0);
        APPEND_OPEN_TAG("<speed level=\"%d\">", level);
        PREPEND_CLOSE_TAG("</speed>");
    }
    if (pitch >= 0.0) {
        int level = (int)ceil(pitch * 100.0);
        APPEND_OPEN_TAG("<pitch level=\"%d\">", level);
        PREPEND_CLOSE_TAG("</pitch>");
    }
    if (volume >= 0.0) {
        int level = (int)ceil(volume * 100.0);
        APPEND_OPEN_TAG("<volume level=\"%d\">", level);
        PREPEND_CLOSE_TAG("</volume>");
    }

#undef APPEND_OPEN_TAG
#undef PREPEND_CLOSE_TAG

    const size_t open_len = strlen(open_tags);
    const size_t close_len = strlen(close_tags);

    if (open_len == 0 && close_len == 0) {
        return (char *)text_utf8; // borrowed
    }

    const size_t text_len = strlen(text_utf8);
    if (open_len > SIZE_MAX - text_len) {
        PyErr_NoMemory();
        return NULL;
    }
    const size_t mid = open_len + text_len;
    if (close_len > SIZE_MAX - mid) {
        PyErr_NoMemory();
        return NULL;
    }
    const size_t total_len = mid + close_len;

    char *synth_text = (char *)malloc(total_len + 1);
    if (!synth_text) {
        PyErr_NoMemory();
        return NULL;
    }

    memcpy(synth_text, open_tags, open_len);
    memcpy(synth_text + open_len, text_utf8, text_len);
    memcpy(synth_text + open_len + text_len, close_tags, close_len);
    synth_text[total_len] = '\0';

    *need_free = 1;
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

// Feed UTF-8 text to Pico in safe chunks, gather PCM bytes in a growing buffer.
static int synthesize_to_pcm(pico_System pico_system, pico_Engine pico_engine,
                             const char *synth_text, unsigned char **out_pcm,
                             size_t *out_pcm_size) {
    *out_pcm = NULL;
    *out_pcm_size = 0;

    unsigned char *pcm_data = NULL;
    size_t pcm_size = 0;
    size_t pcm_capacity = 0;

    pico_Retstring out_msg;
    pico_Status status;

    const unsigned char *inp = (const unsigned char *)synth_text;

    // IMPORTANT: include trailing NUL as end-of-input marker for Pico
    size_t inp_len = strlen((const char *)inp);
    if (inp_len == SIZE_MAX) { // paranoia
        PyErr_SetString(PyExc_RuntimeError, "input too long");
        goto fail;
    }
    inp_len += 1; // include '\0'

    size_t offset = 0;

    pico_Int16 bytes_sent = 0;
    pico_Int16 bytes_recv = 0;
    pico_Int16 out_data_type = 0;
    pico_Int32 get_status = 0;

    unsigned char outbuf[MAX_OUTBUF_SIZE];

    while (offset < inp_len) {
        size_t remaining = inp_len - offset;

        pico_Int16 chunk = (remaining > (size_t)PICO_INT16_MAX)
                               ? (pico_Int16)PICO_INT16_MAX
                               : (pico_Int16)remaining;

        bytes_sent = 0;
        status = pico_putTextUtf8(
            pico_engine, (const pico_Char *)(inp + offset), chunk, &bytes_sent);
        if (status != PICO_OK) {
            pico_getSystemStatusMessage(pico_system, status, out_msg);
            PyErr_Format(PyExc_RuntimeError, "pico_putTextUtf8 failed: %s",
                         out_msg);
            goto fail;
        }

        // bytes_sent should always be positive and <= chunk
        if (bytes_sent <= 0 || bytes_sent > chunk) {
            PyErr_SetString(PyExc_RuntimeError,
                            "pico_putTextUtf8 returned invalid bytes_sent");
            goto fail;
        }

        offset += (size_t)bytes_sent;

        do {
            bytes_recv = 0;
            get_status = pico_getData(pico_engine, (void *)outbuf,
                                      (pico_Int16)MAX_OUTBUF_SIZE, &bytes_recv,
                                      &out_data_type);

            if (get_status != PICO_STEP_BUSY && get_status != PICO_STEP_IDLE) {
                pico_getSystemStatusMessage(pico_system, get_status, out_msg);
                PyErr_Format(PyExc_RuntimeError, "pico_getData failed: %s",
                             out_msg);
                goto fail;
            }

            if (bytes_recv < 0 || bytes_recv > (pico_Int16)MAX_OUTBUF_SIZE) {
                PyErr_SetString(PyExc_RuntimeError,
                                "pico_getData returned invalid bytes_recv");
                goto fail;
            }

            if (bytes_recv > 0) {
                if (!append_pcm(&pcm_data, &pcm_size, &pcm_capacity, outbuf,
                                (size_t)bytes_recv)) {
                    PyErr_NoMemory();
                    goto fail;
                }
            }
        } while (get_status == PICO_STEP_BUSY);
    }

    // Final drain (often still needed)
    do {
        bytes_recv = 0;
        get_status = pico_getData(pico_engine, (void *)outbuf,
                                  (pico_Int16)MAX_OUTBUF_SIZE, &bytes_recv,
                                  &out_data_type);

        if (get_status != PICO_STEP_BUSY && get_status != PICO_STEP_IDLE) {
            pico_getSystemStatusMessage(pico_system, get_status, out_msg);
            PyErr_Format(PyExc_RuntimeError, "pico_getData failed: %s",
                         out_msg);
            goto fail;
        }

        if (bytes_recv < 0 || bytes_recv > (pico_Int16)MAX_OUTBUF_SIZE) {
            PyErr_SetString(PyExc_RuntimeError,
                            "pico_getData returned invalid bytes_recv");
            goto fail;
        }

        if (bytes_recv > 0) {
            if (!append_pcm(&pcm_data, &pcm_size, &pcm_capacity, outbuf,
                            (size_t)bytes_recv)) {
                PyErr_NoMemory();
                goto fail;
            }
        }
    } while (get_status == PICO_STEP_BUSY);

    *out_pcm = pcm_data;
    *out_pcm_size = pcm_size;
    return 1;

fail:
    free(pcm_data);
    return 0;
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

    PyObject *voice_bytes = NULL;
    PyObject *lingware_bytes = NULL;
    PyObject *text_bytes = NULL;

    // Parse: required unicode text + optional objects
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "U|OOOOO", kwlist, &text_obj,
                                     &voice_obj, &speed_obj, &pitch_obj,
                                     &volume_obj, &lingware_obj)) {
        return NULL;
    }

    if (voice_obj != Py_None) {
        if (!PyUnicode_Check(voice_obj)) {
            PyErr_SetString(PyExc_TypeError, "voice must be a string or None");
            return NULL;
        }
        if (!unicode_to_utf8_bytes(voice_obj, &voice_bytes, &voice_cstr)) {
            return NULL;
        }
    }

    if (lingware_obj != Py_None) {
        if (!PyUnicode_Check(lingware_obj)) {
            PyErr_SetString(PyExc_TypeError,
                            "lingware_path must be a string or None");
            Py_XDECREF(voice_bytes);
            return NULL;
        }
        if (!unicode_to_utf8_bytes(lingware_obj, &lingware_bytes,
                                   &lingware_path)) {
            Py_XDECREF(voice_bytes);
            return NULL;
        }
    }

    double speed = -1.0;
    double pitch = -1.0;
    double volume = -1.0;

    if (speed_obj != Py_None) {
        speed = PyFloat_AsDouble(speed_obj);
        if (PyErr_Occurred())
            goto py_fail;
    }
    if (pitch_obj != Py_None) {
        pitch = PyFloat_AsDouble(pitch_obj);
        if (PyErr_Occurred())
            goto py_fail;
    }
    if (volume_obj != Py_None) {
        volume = PyFloat_AsDouble(volume_obj);
        if (PyErr_Occurred())
            goto py_fail;
    }

    const int lang_index = find_voice_index(voice_cstr);
    if (lang_index < 0) {
        PyErr_SetString(PyExc_ValueError, "Unsupported voice");
        goto py_fail;
    }

    const char *text_utf8 = NULL;
    if (!unicode_to_utf8_bytes(text_obj, &text_bytes, &text_utf8)) {
        goto py_fail;
    }

    int synth_need_free = 0;
    char *synth_text =
        build_synth_text(text_utf8, speed, pitch, volume, &synth_need_free);
    if (!synth_text) {
        goto py_fail;
    }

    void *pico_mem = malloc((size_t)PICO_MEM_SIZE);
    if (!pico_mem) {
        PyErr_NoMemory();
        goto c_fail;
    }

    pico_System pico_system = NULL;
    pico_Resource pico_ta = NULL;
    pico_Resource pico_sg = NULL;
    pico_Engine pico_engine = NULL;

    pico_Char *ta_file = NULL;
    pico_Char *sg_file = NULL;

    pico_Char *ta_res_name = NULL;
    pico_Char *sg_res_name = NULL;

    pico_Retstring out_msg;
    pico_Status status;

    status = pico_initialize(pico_mem, (pico_Int32)PICO_MEM_SIZE, &pico_system);
    if (status != PICO_OK) {
        if (pico_system) {
            pico_getSystemStatusMessage(pico_system, status, out_msg);
            PyErr_Format(PyExc_RuntimeError, "pico_initialize failed: %s",
                         out_msg);
        } else {
            PyErr_Format(PyExc_RuntimeError,
                         "pico_initialize failed with status %d", (int)status);
        }
        goto pico_fail;
    }

    ta_file = alloc_path_buf();
    sg_file = alloc_path_buf();
    if (!ta_file || !sg_file) {
        PyErr_NoMemory();
        goto pico_fail;
    }

    if (!format_path(ta_file, lingware_path, pico_internal_ta[lang_index]) ||
        !format_path(sg_file, lingware_path, pico_internal_sg[lang_index])) {
        PyErr_SetString(PyExc_ValueError, "lingware_path too long");
        goto pico_fail;
    }

    status = pico_loadResource(pico_system, ta_file, &pico_ta);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_loadResource ta failed: %s",
                     out_msg);
        goto pico_fail;
    }

    status = pico_loadResource(pico_system, sg_file, &pico_sg);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_loadResource sg failed: %s",
                     out_msg);
        goto pico_fail;
    }

    ta_res_name = (pico_Char *)malloc((size_t)PICO_MAX_RESOURCE_NAME_SIZE);
    sg_res_name = (pico_Char *)malloc((size_t)PICO_MAX_RESOURCE_NAME_SIZE);
    if (!ta_res_name || !sg_res_name) {
        PyErr_NoMemory();
        goto pico_fail;
    }

    status = pico_getResourceName(pico_system, pico_ta, ta_res_name);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_getResourceName ta failed: %s",
                     out_msg);
        goto pico_fail;
    }

    status = pico_getResourceName(pico_system, pico_sg, sg_res_name);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_getResourceName sg failed: %s",
                     out_msg);
        goto pico_fail;
    }

    status = pico_createVoiceDefinition(pico_system, pico_voice_name);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError,
                     "pico_createVoiceDefinition failed: %s", out_msg);
        goto pico_fail;
    }

    status = pico_addResourceToVoiceDefinition(pico_system, pico_voice_name,
                                               ta_res_name);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError,
                     "pico_addResourceToVoiceDefinition ta failed: %s",
                     out_msg);
        goto pico_fail;
    }

    status = pico_addResourceToVoiceDefinition(pico_system, pico_voice_name,
                                               sg_res_name);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError,
                     "pico_addResourceToVoiceDefinition sg failed: %s",
                     out_msg);
        goto pico_fail;
    }

    status = pico_newEngine(pico_system, pico_voice_name, &pico_engine);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_newEngine failed: %s", out_msg);
        goto pico_fail;
    }

    unsigned char *pcm_data = NULL;
    size_t pcm_size = 0;

    if (!synthesize_to_pcm(pico_system, pico_engine, synth_text, &pcm_data,
                           &pcm_size)) {
        goto pico_fail_pcm; // Python error already set
    }

    PyObject *result =
        PyBytes_FromStringAndSize((const char *)pcm_data, (Py_ssize_t)pcm_size);
    if (!result) {
        free(pcm_data);
        goto pico_fail;
    }

    free(pcm_data);

    // Normal cleanup
    if (pico_engine) {
        pico_disposeEngine(pico_system, &pico_engine);
        pico_releaseVoiceDefinition(pico_system, (pico_Char *)pico_voice_name);
    }
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

    if (synth_need_free)
        free(synth_text);
    Py_XDECREF(text_bytes);
    Py_XDECREF(voice_bytes);
    Py_XDECREF(lingware_bytes);

    return result;

pico_fail_pcm:
    // synthesize_to_pcm freed partial buffers on failure
pico_fail:
    if (pico_engine) {
        pico_disposeEngine(pico_system, &pico_engine);
        pico_releaseVoiceDefinition(pico_system, (pico_Char *)pico_voice_name);
    }
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

c_fail:
    if (synth_need_free)
        free(synth_text);
    Py_XDECREF(text_bytes);
    Py_XDECREF(voice_bytes);
    Py_XDECREF(lingware_bytes);
    return NULL;

py_fail:
    Py_XDECREF(text_bytes);
    Py_XDECREF(voice_bytes);
    Py_XDECREF(lingware_bytes);
    return NULL;
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
