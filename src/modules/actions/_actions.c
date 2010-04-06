/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <Python.h>

#include <string.h>

static PyObject *MalformedActionError;
static PyObject *InvalidActionError;

static int
add_to_attrs(PyObject *attrs, PyObject *key, PyObject *attr)
{
	int contains, ret;

	contains = PyDict_Contains(attrs, key);
	if (contains == 0) {
		return (PyDict_SetItem(attrs, key, attr));
	} else if (contains == 1) {
		PyObject *av = PyDict_GetItem(attrs, key);
		Py_INCREF(av);
		if (PyList_Check(av)) {
			ret = PyList_Append(av, attr);
			Py_DECREF(av);
			return (ret);
		} else {
			PyObject *list;
			if ((list = PyList_New(2)) == NULL)
				return (-1);
			PyList_SET_ITEM(list, 0, av);
			Py_INCREF(attr);
			PyList_SET_ITEM(list, 1, attr);
			ret = PyDict_SetItem(attrs, key, list);
			Py_DECREF(list);
			return (ret);
		}
	} else if (contains == -1)
		return (-1);

	/* Shouldn't ever get here */
	return (0);
}

static void *
set_malformederr(const char *str, int pos, const char *msg)
{
	PyObject *val;

	if ((val = Py_BuildValue("sis", str, pos, msg)) != NULL) {
		PyErr_SetObject(MalformedActionError, val);
		Py_DECREF(val);
	}
	return (NULL);
}

static void *
set_invaliderr(const char *str, const char *msg)
{
	PyObject *val;

	if ((val = Py_BuildValue("ss", str, msg)) != NULL) {
		PyErr_SetObject(InvalidActionError, val);
		Py_DECREF(val);
	}
	return (NULL);
}

/*ARGSUSED*/
static PyObject *
_fromstr(PyObject *self, PyObject *args)
{
	char *s, *str, *keystr, *slashmap = NULL;
	int strl;
	int i, ks, vs, keysize;
	char quote;
	PyObject *type = NULL;
	PyObject *hash = NULL;
	PyObject *attrs = NULL;
	PyObject *ret = NULL;
	PyObject *key = NULL;
	PyObject *attr = NULL;
	enum {
		KEY,    /* key            */
		UQVAL,  /* unquoted value */
		QVAL,   /* quoted value   */
		WS      /* whitespace     */
	} state;

#define malformed(msg) set_malformederr(str, i, (msg))
#define invalid(msg) set_invaliderr(str, (msg))
#define CLEANUP_REFS \
	Py_XDECREF(key);\
	Py_XDECREF(type);\
	Py_XDECREF(attr);\
	Py_XDECREF(attrs);\
	Py_XDECREF(hash);

	if (PyArg_ParseTuple(args, "s#", &str, &strl) == 0) {
		PyErr_SetString(PyExc_ValueError, "could not parse argument");
		return (NULL);
	}

	s = strpbrk(str, " \t");

	i = strl;
	if (s == NULL)
		return (malformed("no attributes"));

	if ((type = PyString_FromStringAndSize(str, s - str)) == NULL)
		return (NULL);

	ks = vs = s - str;
	state = WS;
	if ((attrs = PyDict_New()) == NULL) {
		Py_DECREF(type);
		return (NULL);
	}
	for (i = s - str; str[i]; i++) {
		if (state == KEY) {
			keysize = i - ks;
			keystr = &str[ks];

			if (str[i] == ' ' || str[i] == '\t') {
				if (PyDict_Size(attrs) > 0 || hash != NULL) {
					CLEANUP_REFS;
					return (malformed("whitespace in key"));
				}
				else {
					if ((hash = PyString_FromStringAndSize(
						keystr, keysize)) == NULL) {
						CLEANUP_REFS;
						return (NULL);
					}
					state = WS;
				}
			} else if (str[i] == '=') {
				if ((key = PyString_FromStringAndSize(
					keystr, keysize)) == NULL) {
					CLEANUP_REFS;
					return (NULL);
				}

				if (keysize == 4 && strncmp(keystr, "data", keysize) == 0) {
					CLEANUP_REFS;
					return (invalid("invalid key: 'data'"));
				}

				/*
				 * Pool attribute key to reduce memory usage and
				 * potentially improve lookup performance.
				 */
				PyString_InternInPlace(&key);

				if (i == ks) {
					CLEANUP_REFS;
					return (malformed("impossible: missing key"));
				}
				else if (++i == strl) {
					CLEANUP_REFS;
					return (malformed("missing value"));
				}
				if (str[i] == '\'' || str[i] == '\"') {
					state = QVAL;
					quote = str[i];
					vs = i + 1;
				} else if (str[i] == ' ' || str[i] == '\t') {
					CLEANUP_REFS;
					return (malformed("missing value"));
				}
				else {
					state = UQVAL;
					vs = i;
				}
			} else if (str[i] == '\'' || str[i] == '\"') {
				CLEANUP_REFS;
				return (malformed("quote in key"));
			}
		} else if (state == QVAL) {
			if (str[i] == '\\') {
				if (i == strl - 1)
					break;
				/*
				 * "slashmap" is a simple bitmap (bytemap?)
				 * keeping track of what characters are
				 * backslashes that need to be removed
				 * from the final attribute string.
				 * All other bytes are NUL bytes.
				 */
				if (slashmap == NULL) {
					int smlen = strl - (i - vs);
					slashmap = calloc(1, smlen + 1);
					if (slashmap == NULL)
						return (PyErr_NoMemory());
				}
				i++;
				if (str[i] == '\\' || str[i] == quote) {
					slashmap[i - 1 - vs] = '\\';
				}
			} else if (str[i] == quote) {
				state = WS;
				if (slashmap != NULL) {
					char *sattr;
					int j, o, attrlen;

					attrlen = i - vs;
					sattr = calloc(1, attrlen + 1);
					if (sattr == NULL) {
						free(slashmap);
						return (PyErr_NoMemory());
					}
					/*
					 * Copy the attribute from str into
					 * sattr, removing backslashes as
					 * slashmap indicates we should.
					 */
					for (j = 0, o = 0; j < attrlen; j++) {
						if (slashmap[j] == '\\') {
							o++;
							continue;
						}
						sattr[j - o] = str[vs + j];
					}

					free(slashmap);
					slashmap = NULL;

					if ((attr = PyString_FromStringAndSize(
						sattr, attrlen - o)) == NULL) {
						free(sattr);
						CLEANUP_REFS;
						return (NULL);
					}
					free(sattr);
				} else {
					Py_XDECREF(attr);
					if ((attr = PyString_FromStringAndSize(
					    &str[vs], i - vs)) == NULL) {
						CLEANUP_REFS;
						return (NULL);
					}
				}

				if (add_to_attrs(attrs, key, attr) == -1) {
					CLEANUP_REFS;
					return (NULL);
				}
			}
		} else if (state == UQVAL) {
			if (str[i] == ' ' || str[i] == '\t') {
				state = WS;
				Py_XDECREF(attr);
				attr = PyString_FromStringAndSize(&str[vs], i - vs);
				if (add_to_attrs(attrs, key, attr) == -1) {
					CLEANUP_REFS;
					return (NULL);
				}
			}
		} else if (state == WS) {
			if (str[i] != ' ' && str[i] != '\t') {
				state = KEY;
				ks = i;
				if (str[i] == '=') {
					CLEANUP_REFS;
					return (malformed("missing key"));
				}
			}
		}
	}

	if (state == QVAL) {
		if (slashmap != NULL)
			free(slashmap);

		CLEANUP_REFS;
		return (malformed("unfinished quoted value"));
	}
	if (state == KEY) {
		CLEANUP_REFS;
		return (malformed("missing value"));
	}

	if (state == UQVAL) {
		Py_XDECREF(attr);
		attr = PyString_FromStringAndSize(&str[vs], i - vs);
		if (add_to_attrs(attrs, key, attr) == -1) {
			CLEANUP_REFS;
			return (NULL);
		}
	}

	if (hash == NULL)
		hash = Py_None;

	ret = Py_BuildValue("OOO", type, hash, attrs);
	Py_XDECREF(key);
	Py_XDECREF(attr);
	Py_DECREF(type);
	Py_DECREF(attrs);
	if (hash != Py_None)
		Py_DECREF(hash);
	return (ret);
}

static PyMethodDef methods[] = {
	{ "_fromstr", _fromstr, METH_VARARGS },
	{ NULL, NULL }
};

PyMODINIT_FUNC
init_actions(void)
{
	PyObject *sys, *pkg_actions;
	PyObject *sys_modules;

	if (Py_InitModule("_actions", methods) == NULL)
		return;

	/*
	 * We need to retrieve the MalformedActionError object from pkg.actions.
	 * We can't import pkg.actions directly, because that would result in a
	 * circular dependency.  But the "sys" module has a dict called
	 * "modules" which maps loaded module names to the corresponding module
	 * objects.  We can then grab the exception from those objects.
	 */

	if ((sys = PyImport_ImportModule("sys")) == NULL)
		return;

	if ((sys_modules = PyObject_GetAttrString(sys, "modules")) == NULL)
		return;

	if ((pkg_actions = PyDict_GetItemString(sys_modules, "pkg.actions"))
		== NULL) {
		/* No exception is set */
		PyErr_SetString(PyExc_KeyError, "pkg.actions");
		return;
	}

	MalformedActionError = \
		PyObject_GetAttrString(pkg_actions, "MalformedActionError");
	InvalidActionError = \
		PyObject_GetAttrString(pkg_actions, "InvalidActionError");
}
