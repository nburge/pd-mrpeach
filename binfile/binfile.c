/* binfile.c An external for Pure Data that reads and writes binary files
*	Copyright (C) 2007  Martin Peach
*
*	This program is free software; you can redistribute it and/or modify
*	it under the terms of the GNU General Public License as published by
*	the Free Software Foundation; either version 2 of the License, or
*	any later version.
*
*	This program is distributed in the hope that it will be useful,
*	but WITHOUT ANY WARRANTY; without even the implied warranty of
*	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*	GNU General Public License for more details.
*
*	You should have received a copy of the GNU General Public License
*	along with this program; if not, write to the Free Software
*	Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*
*	The latest version of this file can be found at:
*	http://pure-data.cvs.sourceforge.net/pure-data/externals/mrpeach/binfile/
*
*	martin.peach@sympatico.ca
*/

#include "m_pd.h"
#include <stdio.h>
#include <string.h>

static t_class *binfile_class;

#define ALLOC_BLOCK_SIZE 65536 /* number of bytes to add when resizing buffer */

typedef struct t_binfile
{
    t_object    x_obj;
    t_outlet    *x_bin_outlet;
    t_outlet    *x_info_outlet;
    t_outlet    *x_bang_outlet;
    FILE        *x_fP;
    char        x_fPath[MAXPDSTRING];
    char        *x_buf; /* read/write buffer in memory for file contents */
    size_t      x_buf_length; /* current length of buf */
    size_t      x_rd_offset; /* current read offset into the buffer */
    size_t      x_wr_offset; /* current write offset into the buffer */
} t_binfile;

static void binfile_rewind (t_binfile *x);
static void binfile_free(t_binfile *x);
static FILE *binfile_open_path(t_binfile *x, char *path, char *mode);
static void binfile_read(t_binfile *x, t_symbol *path);
static void binfile_write(t_binfile *x, t_symbol *path);
static void binfile_bang(t_binfile *x);
static void binfile_float(t_binfile *x, t_float val);
static void binfile_list(t_binfile *x, t_symbol *s, int argc, t_atom *argv);
static void binfile_add(t_binfile *x, t_symbol *s, int argc, t_atom *argv);
static void binfile_clear(t_binfile *x);
static void binfile_info(t_binfile *x);
static void binfile_set(t_binfile *x, t_symbol *s, int argc, t_atom *argv);
static void *binfile_new(t_symbol *s, int argc, t_atom *argv);
void binfile_setup(void);

void binfile_setup(void)
{
    binfile_class = class_new (gensym("binfile"),
        (t_newmethod) binfile_new,
        (t_method)binfile_free, sizeof(t_binfile),
        CLASS_DEFAULT,
        A_GIMME, 0);

    class_addbang(binfile_class, binfile_bang);
    class_addfloat(binfile_class, binfile_float);
    class_addlist(binfile_class, binfile_list);
    class_addmethod(binfile_class, (t_method)binfile_read, gensym("read"), A_DEFSYMBOL, 0);
    class_addmethod(binfile_class, (t_method)binfile_write, gensym("write"), A_DEFSYMBOL, 0);
    class_addmethod(binfile_class, (t_method)binfile_add, gensym("add"), A_GIMME, 0);
    class_addmethod(binfile_class, (t_method)binfile_set, gensym("set"), A_GIMME, 0);
    class_addmethod(binfile_class, (t_method)binfile_clear, gensym("clear"), 0);
    class_addmethod(binfile_class, (t_method)binfile_rewind, gensym("rewind"), 0);
    class_addmethod(binfile_class, (t_method)binfile_info, gensym("info"), 0);
}

static void *binfile_new(t_symbol *s, int argc, t_atom *argv)
{
    t_binfile  *x = (t_binfile *)pd_new(binfile_class);
    t_symbol    *pathSymbol;
    int         i;

    if (x == NULL)
    {
        error("binfile: Could not create...");
        return x;
    }
    x->x_fP = NULL;
    x->x_fPath[0] = '\0';
    x->x_buf_length = ALLOC_BLOCK_SIZE;
    /* find the first string in the arg list and interpret it as a path to a file */
    for (i = 0; i < argc; ++i)
    {
        if (argv[i].a_type == A_SYMBOL)
        {
            pathSymbol = atom_getsymbol(&argv[i]);
            if (pathSymbol != NULL)
                binfile_read(x, pathSymbol);
        }
    }
    /* find the first float in the arg list and interpret it as the size of the buffer */
    for (i = 0; i < argc; ++i)
    {
        if (argv[i].a_type == A_FLOAT)
        {
            x->x_buf_length = atom_getfloat(&argv[i]);
            break;
        }
    }
    if ((x->x_buf = getbytes(x->x_buf_length)) == NULL)
        error ("binfile: Unable to allocate %lu bytes for buffer", x->x_buf_length);
    x->x_bin_outlet = outlet_new(&x->x_obj, gensym("float"));
    x->x_info_outlet = outlet_new(&x->x_obj, gensym("list"));
    x->x_bang_outlet = outlet_new(&x->x_obj, gensym("bang")); /* bang at end of file */
    return (void *)x;
}

static void binfile_free(t_binfile *x)
{
    if (x->x_buf != NULL)
        freebytes(x->x_buf, x->x_buf_length);
    x->x_buf = NULL;
    x->x_buf_length = 0L;
}

static FILE *binfile_open_path(t_binfile *x, char *path, char *mode)
/* path is a string. Up to PATH_BUF_SIZE-1 characters will be copied into x->x_fPath. */
/* mode should be "rb" or "wb" */
/* x->x_fPath will be used as a file name to open. */
/* binfile_open_path attempts to open the file for binary mode reading. */
/* Returns FILE pointer if successful, else 0. */
{
    FILE    *fP;
    char    tryPath[MAXPDSTRING];

    strncpy(tryPath, path, MAXPDSTRING-1); /* copy path into a length-limited buffer */
    /* ...if it doesn't work we won't mess up x->x_fPath */
    tryPath[MAXPDSTRING-1] = '\0'; /* just make sure there is a null termination */

    return fopen(tryPath, mode);
}

static void binfile_write(t_binfile *x, t_symbol *path)
/* open the file for writing and write the entire buffer to it, then close it */
{
    size_t bytes_written = 0L;

    if (0==(x->x_fP = binfile_open_path(x, path->s_name, "wb")))
        error("binfile: Unable to open %s for writing", path->s_name);
    bytes_written = fwrite(x->x_buf, 1L, x->x_wr_offset, x->x_fP);
    if (bytes_written != x->x_wr_offset) post("binfile: %ld bytes written != %ld", bytes_written, x->x_wr_offset);
    else post("binfile: wrote %ld bytes to %s", bytes_written, path->s_name);
    fclose(x->x_fP);
    x->x_fP = NULL;
}

static void binfile_read(t_binfile *x, t_symbol *path)
/* open the file for reading and load it into the buffer, then close it */
{
    size_t file_length = 0L;
    size_t bytes_read = 0L;

    if (0==(x->x_fP = binfile_open_path(x, path->s_name, "rb")))
    {
        error("binfile: Unable to open %s for reading", path->s_name);
        return;
    }
    /* get length of file */
    while (EOF != getc(x->x_fP)) ++file_length;

    if (file_length == 0L) return;
    /* get storage for file contents */
    if (0 != x->x_buf) freebytes(x->x_buf, x->x_buf_length);
    x->x_buf = getbytes(file_length);
    if (NULL == x->x_buf)
    {
        x->x_buf_length = 0L;
        error ("binfile: unable to allocate %ld bytes for %s", file_length, path->s_name);
        return;
    }
    x->x_rd_offset = 0L;
    /* read file into buf */
    rewind(x->x_fP);
    bytes_read = fread(x->x_buf, 1L, file_length, x->x_fP);
    x->x_buf_length = bytes_read;
    x->x_wr_offset = x->x_buf_length;
    x->x_rd_offset = 0L;
    fclose (x->x_fP);
    x->x_fP = NULL;
    if (bytes_read != file_length) post("binfile length %ld not equal to bytes read (%ld)", file_length, bytes_read);
    else post("binfle: read %ld bytes from %s", bytes_read, path->s_name);
}

static void binfile_bang(t_binfile *x)
/* get the next byte in the file and send it out x_bin_list_outlet */
{
    unsigned char c;

    if (x->x_rd_offset < x->x_wr_offset)
    {
        c = x->x_buf[x->x_rd_offset++];
        if (x->x_rd_offset == x->x_wr_offset) outlet_bang(x->x_bang_outlet);
        outlet_float(x->x_bin_outlet, (float)c);
    }
    else outlet_bang(x->x_bin_outlet);
}

/* The arguments of the ``list''-method
* a pointer to the class-dataspace
* a pointer to the selector-symbol (always &s_list)
* the number of atoms and a pointer to the list of atoms:
*/

static void binfile_add(t_binfile *x, t_symbol *s, int argc, t_atom *argv)
/* add a list of bytes to the buffer */
{
    int         i, j;
    float       f;

    for (i = 0; i < argc; ++i)
    {
        if (A_FLOAT == argv[i].a_type)
        {
            j = atom_getint(&argv[i]);
            f = atom_getfloat(&argv[i]);
            if (j < -128 || j > 255)
            {
                error("binfile: input (%d) out of range [0..255]", j);
                return;
            }
            if (j != f)
            {
                error("binfile: input (%f) not an integer", f);
                return;
            }
            if (x->x_buf_length <= x->x_wr_offset)
            {
                x->x_buf = resizebytes(x->x_buf, x->x_buf_length, x->x_buf_length+ALLOC_BLOCK_SIZE);
                if (x->x_buf == NULL)
                {
                    error("binfile: unable to resize buffer");
                    return;
                }
                x->x_buf_length += ALLOC_BLOCK_SIZE;
            }
            x->x_buf[x->x_wr_offset++] = j;
        }
        else
        {
            error("binfile: input %d not a float", i);
            return;
        }
    }
}

static void binfile_list(t_binfile *x, t_symbol *s, int argc, t_atom *argv)
{
    binfile_add(x, s, argc, argv);
}

static void binfile_set(t_binfile *x, t_symbol *s, int argc, t_atom *argv)
/* clear then add a list of bytes to the buffer */
{
    binfile_clear(x);
    binfile_add(x, s, argc, argv);
}

static void binfile_clear(t_binfile *x)
{
    x->x_wr_offset = 0L;
    x->x_rd_offset = 0L;
}

static void binfile_float(t_binfile *x, t_float val)
/* add a single byte to the file */
{
    t_atom a;

    SETFLOAT(&a, val);
    binfile_add(x, gensym("float"), 1, &a);
}

static void binfile_rewind (t_binfile *x)
{
    x->x_rd_offset = 0L;
}

static void binfile_info(t_binfile *x)
{
    t_atom *output_atom = getbytes(sizeof(t_atom));
    SETFLOAT(output_atom, x->x_buf_length);
    outlet_anything( x->x_info_outlet, gensym("buflength"), 1, output_atom);
    SETFLOAT(output_atom, x->x_rd_offset);
    outlet_anything( x->x_info_outlet, gensym("readoffset"), 1, output_atom);
    SETFLOAT(output_atom, x->x_wr_offset);
    outlet_anything( x->x_info_outlet, gensym("writeoffset"), 1, output_atom);
    freebytes(output_atom,sizeof(t_atom));
}
/* fin binfile.c */