/*
 * Copyright (c) 2012-2013, Steeve Morin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Steeve Morin nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <Python.h>
#include <stdlib.h>
#include <math.h>
#include "lz4.h"
#include "lz4hc.h"

#ifndef Py_UNUSED /* This is already defined for Python 3.4 onwards */
#ifdef __GNUC__
#define Py_UNUSED(name) _unused_ ## name __attribute__((unused))
#else
#define Py_UNUSED(name) _unused_ ## name
#endif
#endif

#if defined(_WIN32) && defined(_MSC_VER)
#define inline __inline
#if _MSC_VER >= 1600
#include <stdint.h>
#else /* _MSC_VER >= 1600 */
typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
#endif /* _MSC_VER >= 1600 */
#endif

#if defined(__SUNPRO_C) || defined(__hpux) || defined(_AIX)
#define inline
#endif

static inline void
store_le32 (char *c, uint32_t x)
{
  c[0] = x & 0xff;
  c[1] = (x >> 8) & 0xff;
  c[2] = (x >> 16) & 0xff;
  c[3] = (x >> 24) & 0xff;
}

static inline uint32_t
load_le32 (const char *c)
{
  const uint8_t *d = (const uint8_t *) c;
  return d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24);
}

#ifdef inline
#undef inline
#endif

static const int hdr_size = sizeof (uint32_t);

typedef enum
{
  DEFAULT,
  FAST,
  HIGH_COMPRESSION
} compression_type;

static PyObject *
py_lz4_compress (PyObject * Py_UNUSED (self), PyObject * args, PyObject * kwds)
{
  PyObject *result;
  static char *argnames[] = { "source", "mode", "acceleration", "compression", NULL };
  const char *source;
  const char *mode = "default";
  int source_size, acceleration = 1, compression = 0;
  char *dest;
  int dest_size;
  compression_type comp;

  if (!PyArg_ParseTupleAndKeywords (args, kwds, "s#|sII", argnames,
				    &source, &source_size,
				    &mode, &acceleration, &compression))
    {
      return NULL;
    }

  if (!strncmp (mode, "default", sizeof ("default")))
    {
      comp = DEFAULT;
    }
  else if (!strncmp (mode, "fast", sizeof ("fast")))
    {
      comp = FAST;
    }
  else if (!strncmp (mode, "high_compression", sizeof ("high_compression")))
    {
      comp = HIGH_COMPRESSION;
    }
  else
    {
      PyErr_Format (PyExc_ValueError,
		    "Invalid mode argument: %s. Must be one of: standard, fast, high_compression",
		    mode);
      return NULL;
    }

  dest_size = hdr_size + LZ4_compressBound (source_size);

  result = PyBytes_FromStringAndSize (NULL, dest_size);
  if (result == NULL)
    {
      return NULL;
    }

  dest = PyBytes_AS_STRING (result);
  store_le32 (dest, source_size);

  if (source_size > 0)
    {
      int osize, actual_size;

      Py_BEGIN_ALLOW_THREADS

      switch (comp)
	{
	case DEFAULT:
	  osize = LZ4_compress_default (source, dest + hdr_size, source_size,
					LZ4_compressBound (source_size));
	  break;
	case FAST:
	  osize = LZ4_compress_fast (source, dest + hdr_size, source_size,
				     LZ4_compressBound (source_size),
				     acceleration);
	  break;
	case HIGH_COMPRESSION:
	  osize = LZ4_compress_HC (source, dest + hdr_size, source_size,
				   LZ4_compressBound (source_size),
				   compression);
	  break;
	}

      actual_size = hdr_size + osize;

      Py_END_ALLOW_THREADS

      /* Resizes are expensive; tolerate some slop to avoid. */
      if (actual_size < (dest_size / 4) * 3)
	{
	  _PyBytes_Resize (&result, actual_size);
	}
      else
	{
	  Py_SIZE (result) = actual_size;
	}
    }

  return result;
}

static PyObject *
py_lz4_decompress (PyObject * Py_UNUSED (self), PyObject * args)
{
  PyObject *result;
  const char *source;
  int source_size;
  uint32_t dest_size;

  if (!PyArg_ParseTuple (args, "s#", &source, &source_size))
    {
      return NULL;
    }

  if (source_size < hdr_size)
    {
      PyErr_SetString (PyExc_ValueError, "Input too short");
      return NULL;
    }

  dest_size = load_le32 (source);

  if (dest_size > INT_MAX)
    {
      PyErr_Format (PyExc_ValueError, "Invalid size in header: 0x%x",
		    dest_size);
      return NULL;
    }

  result = PyBytes_FromStringAndSize (NULL, dest_size);

  if (result != NULL && dest_size > 0)
    {
      char *dest = PyBytes_AS_STRING (result);
      int osize = -1;

      Py_BEGIN_ALLOW_THREADS

      osize =
	LZ4_decompress_safe (source + hdr_size, dest, source_size - hdr_size,
			     dest_size);

      Py_END_ALLOW_THREADS

      if (osize < 0)
	{
	  PyErr_Format (PyExc_ValueError, "Corrupt input at byte %d", -osize);
	  Py_CLEAR (result);
	}
    }

  return result;
}

static PyObject *
py_lz4_versionnumber (PyObject * Py_UNUSED (self), PyObject * Py_UNUSED (args))
{
  return Py_BuildValue ("i", LZ4_versionNumber ());
}

#define COMPRESS_DOCSTRING \
  "compress(source, mode='default', acceleration=1, compression=0)\n\n"	\
  "Compress source, returning the compressed data as a string.\n"	\
  "Raises an exception if any error occurs.\n\n"			\
  "Args:\n"								\
  "    source (str): Data to compress\n"				\
  "    mode (str): If 'default' or unspecified use the default LZ4\n"	\
  "        compression mode. Set to 'fast' to use the fast compression\n" \
  "        LZ4 mode at the expense of compression. Set to\n"		\
  "        'high_compression' to use the LZ4 high-compression mode at\n" \
  "        the exepense of speed\n"					\
  "    acceleration (int): When mode is set to 'fast' this argument\n"	\
  "        specifies the acceleration. The larger the acceleration, the\n" \
  "        faster the but the lower the compression. The default\n"	\
  "        compression corresponds to a value of 1.\n"			\
  "    compression (int): When mode is set to `high_compression` this\n" \
  "        argument specifies the compression. Valid values are between\n" \
  "        0 and 16. Values between 4-9 are recommended, and 0 is the\n" \
  "        default.\n\n"						\
  "Returns:\n"								\
  "    str: Compressed data\n"

#define DECOMPRESS_DOCSTRING \
  "decompress(source)\n\n"					      \
  "Decompress source, returning the uncompressed data as a string.\n" \
  "Raises an exception if any error occurs.\n\n"		      \
  "Args:\n"							      \
  "    source (str): Data to decompress\n\n"			      \
  "Returns:\n"							      \
  "    str: decompressed data\n"

static PyMethodDef Lz4Methods[] = {
  {"compress", (PyCFunction) py_lz4_compress, METH_VARARGS | METH_KEYWORDS,
   COMPRESS_DOCSTRING},
  {"decompress", py_lz4_decompress, METH_VARARGS, DECOMPRESS_DOCSTRING},
  {"lz4version", py_lz4_versionnumber, METH_VARARGS,
   "Returns the version number of the lz4 C library"},
  {NULL, NULL, 0, NULL}
};

#undef COMPRESS_DOCSTRING
#undef DECOMPRESS_DOCSTRING

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef moduledef = {
  PyModuleDef_HEAD_INIT,
  "_block",
  NULL,
  -1,
  Lz4Methods,
  NULL,
  NULL,
  NULL,
  NULL,
};

PyObject *
PyInit__block (void)
{
  PyObject *module = PyModule_Create (&moduledef);

  if (module == NULL)
    {
      return NULL;
    }

  return module;
}

#else /* Python 2 */
PyMODINIT_FUNC
init_block (void)
{
  (void) Py_InitModule ("_block", Lz4Methods);
}
#endif /* PY_MAJOR_VERSION >= 3 */
