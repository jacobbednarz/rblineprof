#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include <ruby.h>
#include <node.h>
#include <intern.h>
#include <st.h>
#include <re.h>

static VALUE gc_hook;

typedef struct {
  char *filename;
  uint64_t *lines;
  long nlines;

  uint64_t last_time;
  long last_line;
} sourcefile_t;

static struct {
  bool enabled;

  // single file mode, store filename and line data directly
  char *source_filename;
  sourcefile_t file;

  // regex mode, store file data in hash table
  VALUE source_regex;
  st_table *files;
}
rblineprof = {
  .enabled = false,
  .source_filename = NULL,
  .source_regex = Qfalse,
  .files = NULL
};

static uint64_t
timeofday_usec()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec*1e6 +
         (uint64_t)tv.tv_usec;
}

static void
profiler_hook(rb_event_t event, NODE *node, VALUE self, ID mid, VALUE klass)
{
  sourcefile_t *sourcefile = NULL;

  char *file = node->nd_file;
  long line  = nd_line(node);

  if (rblineprof.source_filename) { // single file mode
    if (rblineprof.source_filename == file) {
      sourcefile = &rblineprof.file;
      sourcefile->filename = file;
    } else {
      return;
    }

  } else { // regex mode
    st_lookup(rblineprof.files, (st_data_t)file, (st_data_t *)&sourcefile);

    if ((long)sourcefile == Qnil) // known negative match, skip
      return;

    if (!sourcefile) { // unknown file, check against regex
      if (rb_reg_search(rblineprof.source_regex, rb_str_new2(file), 0, 0) >= 0) {
        sourcefile = calloc(1, sizeof(*sourcefile));
        sourcefile->filename = file;
        st_insert(rblineprof.files, (st_data_t)file, (st_data_t)sourcefile);
      } else { // no match, insert Qnil to prevent regex next time
        st_insert(rblineprof.files, (st_data_t)file, (st_data_t)Qnil);
      }
    }
  }

  if (sourcefile) {
    uint64_t now = timeofday_usec();

    if (sourcefile->last_time) {
      /* allocate space for per-line data the first time */
      if (!sourcefile->lines) {
        sourcefile->nlines = line + 100;
        sourcefile->lines = calloc(sourcefile->nlines, sizeof(uint64_t));
      }

      /* grow the per-line array if necessary */
      if (line >= sourcefile->nlines) {
        sourcefile->nlines = line + 100;
        sourcefile->lines = realloc(sourcefile->lines, sizeof(uint64_t) * sourcefile->nlines);
      }

      /* record the sample */
      sourcefile->lines[sourcefile->last_line] += (now - sourcefile->last_time);
    }

    sourcefile->last_time = now;
    sourcefile->last_line = line;
  }
}

static int
gc_mark_files(st_data_t key, st_data_t record, st_data_t arg)
{
  rb_source_filename((char *)record);
  return ST_CONTINUE;
}

static int
cleanup_files(st_data_t key, st_data_t record, st_data_t arg)
{
  sourcefile_t *sourcefile = (sourcefile_t*)record;
  if (!sourcefile || (long)sourcefile == Qnil) return;

  if (sourcefile->lines)
    free(sourcefile->lines);
  free(sourcefile);

  return ST_DELETE;
}

static int
summarize_files(st_data_t key, st_data_t record, st_data_t arg)
{
  VALUE ret = (VALUE)arg;
  VALUE ary = rb_ary_new();
  sourcefile_t *sourcefile = (sourcefile_t*)record;
  long i;

  for (i=0; i<sourcefile->nlines; i++)
    rb_ary_store(ary, i, ULL2NUM(sourcefile->lines[i]));
  rb_hash_aset(ret, rb_str_new2(sourcefile->filename), ary);

  return ST_CONTINUE;
}

VALUE
lineprof(VALUE self, VALUE filename)
{
  if (!rb_block_given_p())
    rb_raise(rb_eArgError, "block required");

  if (rblineprof.enabled)
    rb_raise(rb_eArgError, "profiler is already enabled");

  VALUE filename_class = rb_obj_class(filename);

  if (filename_class == rb_cString) {
    rblineprof.source_filename = rb_source_filename(StringValuePtr(filename));
  } else if (filename_class == rb_cRegexp) {
    rblineprof.source_regex = filename;
    rblineprof.source_filename = NULL;
  } else {
    rb_raise(rb_eArgError, "argument must be String or Regexp");
  }

  // cleanup
  st_foreach(rblineprof.files, cleanup_files, 0);
  if (rblineprof.file.lines) {
    free(rblineprof.file.lines);
    rblineprof.file.lines = NULL;
    rblineprof.file.nlines = 0;
  }

  rblineprof.enabled = true;
  rb_add_event_hook(profiler_hook, RUBY_EVENT_LINE);
  rb_yield(Qnil);
  rb_remove_event_hook(profiler_hook);
  rblineprof.enabled = false;

  VALUE ret = rb_hash_new();
  VALUE ary = Qnil;

  if (rblineprof.source_filename) {
    long i;
    ary = rb_ary_new();
    for (i=0; i<rblineprof.file.nlines; i++)
      rb_ary_store(ary, i, ULL2NUM(rblineprof.file.lines[i]));
    rb_hash_aset(ret, rb_str_new2(rblineprof.source_filename), ary);
  } else {
    st_foreach(rblineprof.files, summarize_files, ret);
  }

  return ret;
}

static void
lineprof_mark()
{
  if (rblineprof.enabled) {
    if (rblineprof.source_filename)
      rb_source_filename(rblineprof.source_filename);
    else
      st_foreach(rblineprof.files, gc_mark_files, 0);
  }
}

void
Init_rblineprof()
{
  rblineprof.files = st_init_numtable();

  gc_hook = Data_Wrap_Struct(rb_cObject, lineprof_mark, NULL, NULL);
  rb_global_variable(&gc_hook);

  rb_define_global_function("lineprof", lineprof, 1);
}
