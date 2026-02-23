#define Py_LIMITED_API 0x03090000
#include <Python.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <picoapi.h>
#include <picoapid.h>
#include <picoos.h>

#define PICO_MEM_SIZE 2500000
#define MAX_OUTBUF_SIZE 128

static const char *pico_supported_lang[] = {
    "en-US", "en-GB", "de-DE", "es-ES", "fr-FR", "it-IT"
};
static const char *pico_internal_ta[] = {
    "en-US_ta.bin", "en-GB_ta.bin", "de-DE_ta.bin",
    "es-ES_ta.bin", "fr-FR_ta.bin", "it-IT_ta.bin"
};
static const char *pico_internal_sg[] = {
    "en-US_lh0_sg.bin", "en-GB_kh0_sg.bin", "de-DE_gl0_sg.bin",
    "es-ES_zl0_sg.bin", "fr-FR_nk0_sg.bin", "it-IT_cm0_sg.bin"
};
static const int pico_num_supported = 6;
static const char *pico_voice_name = "PicoVoice";

static int find_voice_index(const char *voice) {
    int i;
    for (i = 0; i < pico_num_supported; i++) {
        if (strcmp(pico_supported_lang[i], voice) == 0) {
            return i;
        }
    }
    return -1;
}

static int unicode_to_utf8_bytes(PyObject *obj, PyObject **out_bytes, const char **out_cstr) {
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
    size_t needed = *size + data_len;
    if (needed > *capacity) {
        size_t new_cap = *capacity == 0 ? 4096 : *capacity;
        while (new_cap < needed) {
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

static void write_le16(unsigned char *dst, unsigned int value) {
    dst[0] = (unsigned char)(value & 0xff);
    dst[1] = (unsigned char)((value >> 8) & 0xff);
}

static void write_le32(unsigned char *dst, unsigned int value) {
    dst[0] = (unsigned char)(value & 0xff);
    dst[1] = (unsigned char)((value >> 8) & 0xff);
    dst[2] = (unsigned char)((value >> 16) & 0xff);
    dst[3] = (unsigned char)((value >> 24) & 0xff);
}

static PyObject *nanotts_speak(PyObject *self, PyObject *args, PyObject *kwargs) {
    static char *kwlist[] = {"text", "voice", "speed", "pitch", "volume", "lingware_path", NULL};
    PyObject *text_obj = NULL;
    PyObject *voice_obj = Py_None;
    PyObject *speed_obj = Py_None;
    PyObject *pitch_obj = Py_None;
    PyObject *volume_obj = Py_None;
    PyObject *lingware_obj = Py_None;

    const char *voice_cstr = "en-GB";
    const char *lingware_path = "./lang";
    PyObject *voice_bytes = NULL;
    PyObject *lingware_bytes = NULL;
    PyObject *text_bytes = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "U|OOOOO", kwlist,
                                     &text_obj, &voice_obj, &speed_obj,
                                     &pitch_obj, &volume_obj, &lingware_obj)) {
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
            PyErr_SetString(PyExc_TypeError, "lingware_path must be a string or None");
            return NULL;
        }
        if (!unicode_to_utf8_bytes(lingware_obj, &lingware_bytes, &lingware_path)) {
            return NULL;
        }
    }

    double speed = -1.0;
    double pitch = -1.0;
    double volume = -1.0;

    if (speed_obj != Py_None) {
        speed = PyFloat_AsDouble(speed_obj);
        if (PyErr_Occurred()) return NULL;
    }
    if (pitch_obj != Py_None) {
        pitch = PyFloat_AsDouble(pitch_obj);
        if (PyErr_Occurred()) return NULL;
    }
    if (volume_obj != Py_None) {
        volume = PyFloat_AsDouble(volume_obj);
        if (PyErr_Occurred()) return NULL;
    }

    int lang_index = find_voice_index(voice_cstr);
    if (lang_index < 0) {
        PyErr_SetString(PyExc_ValueError, "Unsupported voice");
        return NULL;
    }

    const char *text_utf8 = NULL;
    if (!unicode_to_utf8_bytes(text_obj, &text_bytes, &text_utf8)) {
        Py_XDECREF(voice_bytes);
        Py_XDECREF(lingware_bytes);
        return NULL;
    }

    char open_tags[128];
    char close_tags[128];
    open_tags[0] = '\0';
    close_tags[0] = '\0';

    if (speed >= 0.0) {
        char buf[64];
        char new_close[128];
        int level = (int)ceil(speed * 100.0);
        snprintf(buf, sizeof(buf), "<speed level=\"%d\">", level);
        strncat(open_tags, buf, sizeof(open_tags) - strlen(open_tags) - 1);
        snprintf(buf, sizeof(buf), "</speed>");
        snprintf(new_close, sizeof(new_close), "%s%s", buf, close_tags);
        strncpy(close_tags, new_close, sizeof(close_tags) - 1);
        close_tags[sizeof(close_tags) - 1] = '\0';
    }
    if (pitch >= 0.0) {
        char buf[64];
        char new_close[128];
        int level = (int)ceil(pitch * 100.0);
        snprintf(buf, sizeof(buf), "<pitch level=\"%d\">", level);
        strncat(open_tags, buf, sizeof(open_tags) - strlen(open_tags) - 1);
        snprintf(buf, sizeof(buf), "</pitch>");
        snprintf(new_close, sizeof(new_close), "%s%s", buf, close_tags);
        strncpy(close_tags, new_close, sizeof(close_tags) - 1);
        close_tags[sizeof(close_tags) - 1] = '\0';
    }
    if (volume >= 0.0) {
        char buf[64];
        char new_close[128];
        int level = (int)ceil(volume * 100.0);
        snprintf(buf, sizeof(buf), "<volume level=\"%d\">", level);
        strncat(open_tags, buf, sizeof(open_tags) - strlen(open_tags) - 1);
        snprintf(buf, sizeof(buf), "</volume>");
        snprintf(new_close, sizeof(new_close), "%s%s", buf, close_tags);
        strncpy(close_tags, new_close, sizeof(close_tags) - 1);
        close_tags[sizeof(close_tags) - 1] = '\0';
    }

    size_t open_len = strlen(open_tags);
    size_t close_len = strlen(close_tags);
    size_t text_len = strlen(text_utf8);

    char *synth_text = NULL;
    if (open_len == 0 && close_len == 0) {
        synth_text = (char *)text_utf8;
    } else {
        size_t total_len = open_len + text_len + close_len;
        synth_text = (char *)malloc(total_len + 1);
        if (!synth_text) {
            PyErr_NoMemory();
            return NULL;
        }
        memcpy(synth_text, open_tags, open_len);
        memcpy(synth_text + open_len, text_utf8, text_len);
        memcpy(synth_text + open_len + text_len, close_tags, close_len);
        synth_text[total_len] = '\0';
    }

    void *pico_mem = malloc(PICO_MEM_SIZE);
    if (!pico_mem) {
        if (synth_text != text_utf8) free(synth_text);
        Py_XDECREF(text_bytes);
        Py_XDECREF(voice_bytes);
        Py_XDECREF(lingware_bytes);
        PyErr_NoMemory();
        return NULL;
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

    status = pico_initialize(pico_mem, PICO_MEM_SIZE, &pico_system);
    if (status != PICO_OK) {
        if (pico_system) {
            pico_getSystemStatusMessage(pico_system, status, out_msg);
            PyErr_Format(PyExc_RuntimeError, "pico_initialize failed: %s", out_msg);
        } else {
            PyErr_Format(PyExc_RuntimeError, "pico_initialize failed with status %d", (int)status);
        }
        goto cleanup;
    }

    ta_file = (pico_Char *)malloc(PICO_MAX_DATAPATH_NAME_SIZE + PICO_MAX_FILE_NAME_SIZE);
    sg_file = (pico_Char *)malloc(PICO_MAX_DATAPATH_NAME_SIZE + PICO_MAX_FILE_NAME_SIZE);
    if (!ta_file || !sg_file) {
        PyErr_NoMemory();
        goto cleanup;
    }

    snprintf((char *)ta_file, PICO_MAX_DATAPATH_NAME_SIZE + PICO_MAX_FILE_NAME_SIZE,
             "%s/%s", lingware_path, pico_internal_ta[lang_index]);
    snprintf((char *)sg_file, PICO_MAX_DATAPATH_NAME_SIZE + PICO_MAX_FILE_NAME_SIZE,
             "%s/%s", lingware_path, pico_internal_sg[lang_index]);

    status = pico_loadResource(pico_system, ta_file, &pico_ta);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_loadResource ta failed: %s", out_msg);
        goto cleanup;
    }

    status = pico_loadResource(pico_system, sg_file, &pico_sg);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_loadResource sg failed: %s", out_msg);
        goto cleanup;
    }

    ta_res_name = (pico_Char *)malloc(PICO_MAX_RESOURCE_NAME_SIZE);
    sg_res_name = (pico_Char *)malloc(PICO_MAX_RESOURCE_NAME_SIZE);
    if (!ta_res_name || !sg_res_name) {
        PyErr_NoMemory();
        goto cleanup;
    }

    status = pico_getResourceName(pico_system, pico_ta, (char *)ta_res_name);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_getResourceName ta failed: %s", out_msg);
        goto cleanup;
    }

    status = pico_getResourceName(pico_system, pico_sg, (char *)sg_res_name);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_getResourceName sg failed: %s", out_msg);
        goto cleanup;
    }

    status = pico_createVoiceDefinition(pico_system, (const pico_Char *)pico_voice_name);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_createVoiceDefinition failed: %s", out_msg);
        goto cleanup;
    }

    status = pico_addResourceToVoiceDefinition(pico_system, (const pico_Char *)pico_voice_name, ta_res_name);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_addResourceToVoiceDefinition ta failed: %s", out_msg);
        goto cleanup;
    }

    status = pico_addResourceToVoiceDefinition(pico_system, (const pico_Char *)pico_voice_name, sg_res_name);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_addResourceToVoiceDefinition sg failed: %s", out_msg);
        goto cleanup;
    }

    status = pico_newEngine(pico_system, (const pico_Char *)pico_voice_name, &pico_engine);
    if (status != PICO_OK) {
        pico_getSystemStatusMessage(pico_system, status, out_msg);
        PyErr_Format(PyExc_RuntimeError, "pico_newEngine failed: %s", out_msg);
        goto cleanup;
    }

    pico_Char *inp = (pico_Char *)synth_text;
    pico_Int16 bytes_sent = 0;
    pico_Int16 bytes_recv = 0;
    pico_Int16 text_remaining = (pico_Int16)(strlen((const char *)inp) + 1);
    pico_Int16 out_data_type;
    pico_Int32 get_status;
    short outbuf[MAX_OUTBUF_SIZE / 2];

    unsigned char *pcm_data = NULL;
    size_t pcm_size = 0;
    size_t pcm_capacity = 0;

    while (text_remaining > 0) {
        status = pico_putTextUtf8(pico_engine, inp, text_remaining, &bytes_sent);
        if (status != PICO_OK) {
            pico_getSystemStatusMessage(pico_system, status, out_msg);
            PyErr_Format(PyExc_RuntimeError, "pico_putTextUtf8 failed: %s", out_msg);
            goto cleanup_pcm;
        }

        text_remaining -= bytes_sent;
        inp += bytes_sent;

        do {
            get_status = pico_getData(pico_engine, (void *)outbuf,
                                      MAX_OUTBUF_SIZE, &bytes_recv, &out_data_type);
            if (get_status != PICO_STEP_BUSY && get_status != PICO_STEP_IDLE) {
                pico_getSystemStatusMessage(pico_system, get_status, out_msg);
                PyErr_Format(PyExc_RuntimeError, "pico_getData failed: %s", out_msg);
                goto cleanup_pcm;
            }
            if (bytes_recv > 0) {
                if (!append_pcm(&pcm_data, &pcm_size, &pcm_capacity,
                                (const unsigned char *)outbuf, (size_t)bytes_recv)) {
                    PyErr_NoMemory();
                    goto cleanup_pcm;
                }
            }
        } while (get_status == PICO_STEP_BUSY);
    }

    {
        PyObject *result = PyBytes_FromStringAndSize(
            (const char *)pcm_data, (Py_ssize_t)pcm_size);
        if (!result) {
            goto cleanup_pcm;
        }

        free(pcm_data);
        if (synth_text != text_utf8) free(synth_text);
        Py_XDECREF(text_bytes);
        Py_XDECREF(voice_bytes);
        Py_XDECREF(lingware_bytes);
        free(ta_file);
        free(sg_file);
        free(ta_res_name);
        free(sg_res_name);
        if (pico_engine) {
            pico_disposeEngine(pico_system, &pico_engine);
            pico_releaseVoiceDefinition(pico_system, (pico_Char *)pico_voice_name);
        }
        if (pico_ta) pico_unloadResource(pico_system, &pico_ta);
        if (pico_sg) pico_unloadResource(pico_system, &pico_sg);
        if (pico_system) pico_terminate(&pico_system);
        free(pico_mem);
        return result;
    }

cleanup_pcm:
    free(pcm_data);
cleanup:
    if (synth_text != text_utf8) free(synth_text);
    Py_XDECREF(text_bytes);
    Py_XDECREF(voice_bytes);
    Py_XDECREF(lingware_bytes);
    free(ta_file);
    free(sg_file);
    free(ta_res_name);
    free(sg_res_name);
    if (pico_engine) {
        pico_disposeEngine(pico_system, &pico_engine);
        pico_releaseVoiceDefinition(pico_system, (pico_Char *)pico_voice_name);
    }
    if (pico_ta) pico_unloadResource(pico_system, &pico_ta);
    if (pico_sg) pico_unloadResource(pico_system, &pico_sg);
    if (pico_system) pico_terminate(&pico_system);
    if (pico_mem) free(pico_mem);
    return NULL;
}

static PyMethodDef NanottsMethods[] = {
    {"speak", (PyCFunction)nanotts_speak, METH_VARARGS | METH_KEYWORDS, "Synthesize speech to WAV bytes."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef nanotts_module = {
    PyModuleDef_HEAD_INIT,
    "_nanotts",
    "NanoTTS C extension",
    -1,
    NanottsMethods
};

PyMODINIT_FUNC PyInit__nanotts(void) {
    return PyModule_Create(&nanotts_module);
}
