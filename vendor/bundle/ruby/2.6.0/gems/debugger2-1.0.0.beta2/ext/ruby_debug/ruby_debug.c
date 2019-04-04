#include <stdio.h>
#include <ctype.h>
#include "ruby_debug.h"

#define DEBUG_VERSION "0.11"

static VALUE debug_stop(VALUE);
static void context_suspend_0(debug_context_t *);
static void context_resume_0(debug_context_t *);

typedef struct {
    st_table *tbl;
} threads_table_t;

static VALUE tracing            = Qfalse;
static VALUE locker             = Qnil;
static VALUE post_mortem        = Qfalse;
static VALUE keep_frame_binding = Qfalse;
static VALUE track_frame_args   = Qfalse;

static VALUE debug_debugger     = Qfalse;
static const VALUE debug_debugger_stack_size = Qfalse;

static VALUE last_context = Qnil;
static VALUE last_thread  = Qnil;
static VALUE tracepoints = Qnil;

static debug_context_t *last_debug_context = NULL;

VALUE rdebug_threads_tbl = Qnil; /* Context for each of the threads */
VALUE mDebugger;                 /* Ruby Debugger Module object */

static VALUE cThreadsTable;
static VALUE cContext;
static VALUE cDebugThread;

static int start_count = 0;
static int thnum_max = 0;
static int bkp_count = 0;
static int last_debugged_thnum = -1;
static unsigned long last_check = 0;
static unsigned long hook_count = 0;

typedef struct locked_thread_t {
    VALUE thread_id;
    struct locked_thread_t *next;
} locked_thread_t;

static locked_thread_t *locked_head = NULL;
static locked_thread_t *locked_tail = NULL;

static void
reset_stepping_stop_points(debug_context_t *debug_context)
{
    debug_context->dest_frame = -1;
    debug_context->stop_line  = -1;
    debug_context->stop_next  = -1;
}

static VALUE
ref2id(VALUE obj)
{
    return obj;
}

static VALUE
id2ref(VALUE id)
{
    return id;
}

static VALUE
context_thread_0(debug_context_t *debug_context)
{
    return id2ref(debug_context->thread_id);
}

#define ruby_threadptr_data_type *threadptr_data_type()

#define ruby_current_thread ((rb_thread_t *)RTYPEDDATA_DATA(rb_thread_current()))

static int
is_in_locked(VALUE thread_id)
{
    locked_thread_t *node;

    if(!locked_head)
        return 0;

    for(node = locked_head; node != locked_tail; node = node->next)
    {
        if(node->thread_id == thread_id) return 1;
    }
    return 0;
}

static void
add_to_locked(VALUE thread)
{
    locked_thread_t *node;
    VALUE thread_id = ref2id(thread);

    if(is_in_locked(thread_id))
        return;

    node = ALLOC(locked_thread_t);
    node->thread_id = thread_id;
    node->next = NULL;
    if(locked_tail)
        locked_tail->next = node;
    locked_tail = node;
    if(!locked_head)
        locked_head = node;
}

static VALUE
remove_from_locked(void)
{
    VALUE thread;
    locked_thread_t *node;

    if(locked_head == NULL)
        return Qnil;
    node = locked_head;
    locked_head = locked_head->next;
    if(locked_tail == node)
        locked_tail = NULL;
    thread = id2ref(node->thread_id);
    xfree(node);
    return thread;
}

static int
threads_table_mark_keyvalue(st_data_t key, st_data_t value, st_data_t tbl)
{
    VALUE thread = id2ref((VALUE)key);
    if (!value)
        return ST_CONTINUE;

    rb_gc_mark((VALUE)value);
    rb_gc_mark(thread);

    return ST_CONTINUE;
}

static void
threads_table_mark(void* data)
{
    threads_table_t *threads_table = (threads_table_t*)data;
    st_table *tbl = threads_table->tbl;
    st_foreach(tbl, threads_table_mark_keyvalue, (st_data_t)tbl);
}

static void
threads_table_free(void* data)
{
    threads_table_t *threads_table = (threads_table_t*)data;
    st_free_table(threads_table->tbl);
    xfree(threads_table);
}

static VALUE
threads_table_create(void)
{
    threads_table_t *threads_table;

    threads_table = ALLOC(threads_table_t);
    threads_table->tbl = st_init_numtable();
    return Data_Wrap_Struct(cThreadsTable, threads_table_mark, threads_table_free, threads_table);
}

static void
threads_table_clear(VALUE table)
{
    threads_table_t *threads_table;

    Data_Get_Struct(table, threads_table_t, threads_table);
    st_clear(threads_table->tbl);
}

static int
is_living_thread(VALUE thread)
{
    return rb_funcall(thread, rb_intern("alive?"), 0) == Qtrue;
}

static int
threads_table_check_i(st_data_t key, st_data_t value, st_data_t dummy)
{
    VALUE thread;

    if(!value)
    {
        return ST_DELETE;
    }
    thread = id2ref((VALUE)key);
    if(!is_living_thread(thread))
    {
        return ST_DELETE;
    }
    return ST_CONTINUE;
}

static void
check_thread_contexts(void)
{
    threads_table_t *threads_table;

    Data_Get_Struct(rdebug_threads_tbl, threads_table_t, threads_table);
    st_foreach(threads_table->tbl, threads_table_check_i, 0);
}

/*
 *   call-seq:
 *      Debugger.started? -> bool
 *
 *   Returns +true+ the debugger is started.
 */
static VALUE
debug_is_started(VALUE self)
{
    return IS_STARTED ? Qtrue : Qfalse;
}

static void
debug_context_mark(void *data)
{
    debug_context_t *debug_context = (debug_context_t *)data;
    rb_gc_mark(debug_context->breakpoint);
    rb_gc_mark(debug_context->inspected_frame);
}

static void
debug_context_free(void *data)
{
}

static int
exact_stack_size(VALUE thread)
{
    VALUE locs = rb_funcall(thread, rb_intern("backtrace_locations"), 1, INT2FIX(1));
    int stack_size = (int)RARRAY_LEN(locs);
    if (debug_debugger_stack_size && debug_debugger) {
	fprintf(stderr, "[debug:stacksize] %d\n", stack_size);
	rb_p(locs);
    }
    return stack_size;
}

static VALUE
debug_context_create(VALUE thread)
{
    debug_context_t *debug_context;

    debug_context = ALLOC(debug_context_t);

    debug_context->thnum = ++thnum_max;
    debug_context->last_file = Qnil;
    debug_context->last_line = Qnil;
    debug_context->flags = 0;
    debug_context->calced_stack_size = exact_stack_size(thread);

    debug_context->stop_next = -1;
    debug_context->dest_frame = -1;
    debug_context->stop_line = -1;
    debug_context->stop_frame = -1;

    debug_context->stop_reason = CTX_STOP_NONE;
    debug_context->thread_id = ref2id(thread);
    debug_context->breakpoint = Qnil;
    debug_context->inspected_frame = Qnil;

    if (rb_obj_class(thread) == cDebugThread) {
	CTX_FL_SET(debug_context, CTX_FL_IGNORE);
    }
    return Data_Wrap_Struct(cContext, debug_context_mark, debug_context_free, debug_context);
}

static VALUE
debug_context_dup(debug_context_t *debug_context, VALUE self)
{
    debug_context_t *new_debug_context;

    new_debug_context = ALLOC(debug_context_t);
    memcpy(new_debug_context, debug_context, sizeof(debug_context_t));
    new_debug_context->stop_next = -1;
    new_debug_context->dest_frame = -1;
    new_debug_context->stop_line = -1;
    new_debug_context->stop_frame = -1;
    new_debug_context->breakpoint = Qnil;
    new_debug_context->inspected_frame = debug_context->inspected_frame;
    CTX_FL_SET(new_debug_context, CTX_FL_DEAD);

    return Data_Wrap_Struct(cContext, debug_context_mark, debug_context_free, new_debug_context);
}

static void
thread_context_lookup(VALUE thread, VALUE *context, debug_context_t **debug_context, int create)
{
    threads_table_t *threads_table;
    VALUE thread_id;
    debug_context_t *l_debug_context;

    debug_check_started();

    if (last_thread == thread && last_context != Qnil) {
	*context = last_context;
	if (debug_context) {
	    *debug_context = last_debug_context;
	}
	return;
    }
    thread_id = ref2id(thread);
    Data_Get_Struct(rdebug_threads_tbl, threads_table_t, threads_table);
    if (!st_lookup(threads_table->tbl, thread_id, context) || !*context) {
        if (create) {
	    *context = debug_context_create(thread);
	    st_insert(threads_table->tbl, thread_id, *context);
	}
        else {
	    *context = 0;
	    if (debug_context) {
		*debug_context = NULL;
	    }
	    return;
	}
    }

    Data_Get_Struct(*context, debug_context_t, l_debug_context);
    if (debug_context) {
	*debug_context = l_debug_context;
    }

    last_thread = thread;
    last_context = *context;
    last_debug_context = l_debug_context;
}

static VALUE
dc_inspected_frame(const debug_context_t *debug_context)
{
    if (NIL_P(debug_context->inspected_frame)) {
	rb_raise(rb_eRuntimeError, "Inspected frame information is not available");
    }

    return debug_context->inspected_frame;
}

static VALUE
dc_inspected_frame_get(const debug_context_t *debug_context, int frame_index, enum inspected_frame_type type)
{
    VALUE frame = rb_ary_entry(dc_inspected_frame(debug_context), frame_index);
    return rb_ary_entry(frame, type);
}

static VALUE
dc_inspected_frame_location(const debug_context_t *debug_context, int frame_index)
{
    return dc_inspected_frame_get(debug_context, frame_index, INSPECTED_FRAME_LOCATION);
}

static VALUE
dc_inspected_frame_self(const debug_context_t *debug_context, int frame_index)
{
    return dc_inspected_frame_get(debug_context, frame_index, INSPECTED_FRAME_SELF);
}

static VALUE
dc_inspected_frame_class(const debug_context_t *debug_context, int frame_index)
{
    return dc_inspected_frame_get(debug_context, frame_index, INSPECTED_FRAME_CLASS);
}

static VALUE
dc_inspected_frame_binding(const debug_context_t *debug_context, int frame_index)
{
    return dc_inspected_frame_get(debug_context, frame_index, INSPECTED_FRAME_BIDING);
}

#if 0 /* unused */
static VALUE
dc_inspected_frame_iseq(const debug_context_t *debug_context, int frame_index)
{
    return dc_inspected_frame_get(debug_context, frame_index, INSPECTED_FRAME_ISEQ);
}
#endif

static int
dc_stack_size(const debug_context_t *debug_context)
{
    if (!NIL_P(debug_context->inspected_frame)) {
	int stack_size = (int)RARRAY_LEN(debug_context->inspected_frame);

	/* for debug */
	if (0 && debug_debugger && stack_size != debug_context->calced_stack_size) {
	    rb_p(debug_context->inspected_frame);
	    rb_bug("dc_stack_size: stack size calculation miss: calced %d but %d",
		   debug_context->calced_stack_size, stack_size);
	}

	return stack_size;
    }
    else {
	/* TOOD: optimize */
	if (0 && debug_debugger) {
	    int stack_size = exact_stack_size(rb_thread_current());
	    if (stack_size != debug_context->calced_stack_size) {
		rb_bug("dc_stack_size: stack size calculation miss: calced %d but %d",
		       debug_context->calced_stack_size, stack_size);
	    }
	}
	return debug_context->calced_stack_size;
    }
}

static void
halt_while_other_thread_is_active(VALUE current_thread, debug_context_t *debug_context)
{
    while(1) {
	/* halt execution of the current thread if the debugger
           is activated in another
	 */
	while(locker != Qnil && locker != current_thread) {
	    add_to_locked(current_thread);
	    rb_thread_stop();
	}

	/* stop the current thread if it's marked as suspended */
	if(CTX_FL_TEST(debug_context, CTX_FL_SUSPEND) && locker != current_thread) {
	    CTX_FL_SET(debug_context, CTX_FL_WAS_RUNNING);
	    rb_thread_stop();
	}
	else break;
    }
}

static void
trace_cleanup(debug_context_t *debug_context)
{
    VALUE next_thread;
    debug_context->stop_reason = CTX_STOP_NONE;

    /* check that all contexts point to alive threads */
    if (hook_count - last_check > 3000) {
	check_thread_contexts();
	last_check = hook_count;
    }

    /* release a lock */
    locker = Qnil;

    /* let the next thread to run */
    next_thread = remove_from_locked();
    if(next_thread != Qnil) {
	rb_thread_run(next_thread);
    }
}

static void
trace_debug_print(rb_trace_arg_t *trace_arg, debug_context_t *debug_context)
{
    if (debug_debugger == Qtrue) {
	VALUE path  = rb_tracearg_path(trace_arg);
	VALUE line  = rb_tracearg_lineno(trace_arg);
	VALUE event = rb_tracearg_event(trace_arg);
	VALUE mid   = rb_tracearg_method_id(trace_arg);
	fprintf(stderr, "%*s[debug:event#%d] %s@%s:%d %s\n",
		debug_context->calced_stack_size, "",
		debug_context->thnum,
		rb_id2name(SYM2ID(event)),
		RSTRING_PTR(path),
		NUM2INT(line),
		NIL_P(mid) ? "" : rb_id2name(SYM2ID(mid))
		);
    }
}

#define TRACE_SETUP \
  rb_trace_arg_t *trace_arg = rb_tracearg_from_tracepoint(tpval); \
  VALUE current_thread = rb_thread_current(); \
  debug_context_t *debug_context; \
  VALUE context; \
  thread_context_lookup(current_thread, &context, &debug_context, 1); \
  trace_debug_print(trace_arg, debug_context);

#define TRACE_COMMON() \
  if (trace_common(trace_arg, debug_context, current_thread) == 0) { return; }

static int
trace_common(rb_trace_arg_t *trace_arg, debug_context_t *debug_context, VALUE current_thread)
{
    hook_count++;

    if (CTX_FL_TEST(debug_context, CTX_FL_IGNORE)) return 0;

    halt_while_other_thread_is_active(current_thread, debug_context);

    if (locker != Qnil) return 0;

    locker = current_thread;

    /* ignore a skipped section of code */
    if (CTX_FL_TEST(debug_context, CTX_FL_SKIPPED)) {
	trace_cleanup(debug_context);
	return 0;
    }

    /* Sometimes duplicate RUBY_EVENT_LINE messages get generated by the compiler.
     * Ignore them. */
    if (0 /* TODO: check was emitted */) {
	trace_cleanup(debug_context);
	return 0;
    }

    /* There can be many event calls per line, but we only want *one* breakpoint per line. */
    if (debug_context->last_line != rb_tracearg_lineno(trace_arg) ||
	debug_context->last_file != rb_tracearg_path(trace_arg)) {
	CTX_FL_SET(debug_context, CTX_FL_ENABLE_BKPT);
    }

    return 1;
}

struct call_with_inspection_data {
    debug_context_t *debug_context;
    VALUE context;
    ID id;
    int argc;
    VALUE *argv;
};

static VALUE
open_debug_inspector_i(const rb_debug_inspector_t *inspector, void *data)
{
    struct call_with_inspection_data *cwi = (struct call_with_inspection_data *)data;
    VALUE inspected_frame = rb_ary_new();
    VALUE locs = rb_debug_inspector_backtrace_locations(inspector);
    int i;

    for (i=0; i<RARRAY_LEN(locs); i++) {
	VALUE frame = rb_ary_new();
	rb_ary_push(frame, rb_ary_entry(locs, i));
	rb_ary_push(frame, rb_debug_inspector_frame_self_get(inspector, i));
	rb_ary_push(frame, rb_debug_inspector_frame_class_get(inspector, i));
	rb_ary_push(frame, rb_debug_inspector_frame_binding_get(inspector, i));
	rb_ary_push(frame, rb_debug_inspector_frame_iseq_get(inspector, i));

	rb_ary_push(inspected_frame, frame);
    }

    cwi->debug_context->inspected_frame = inspected_frame;
    return rb_funcall2(cwi->context, cwi->id, cwi->argc, cwi->argv);
}

static VALUE
open_debug_inspector(struct call_with_inspection_data *cwi)
{
    return rb_debug_inspector_open(open_debug_inspector_i, cwi);
}

static VALUE
close_debug_inspector(struct call_with_inspection_data *cwi)
{
    cwi->debug_context->inspected_frame = Qnil;
    return Qnil;
}

static VALUE
call_with_debug_inspector(struct call_with_inspection_data *data)
{
    return rb_ensure(open_debug_inspector, (VALUE)data, close_debug_inspector, (VALUE)data);
}

static void
save_current_position(debug_context_t *debug_context, VALUE file, VALUE line)
{
    debug_context->last_file = file;
    debug_context->last_line = line;
    CTX_FL_UNSET(debug_context, CTX_FL_ENABLE_BKPT);
    CTX_FL_UNSET(debug_context, CTX_FL_STEPPED);
    CTX_FL_UNSET(debug_context, CTX_FL_FORCE_MOVE);
}

static VALUE
call_at(VALUE context, debug_context_t *debug_context, ID mid, int argc, VALUE a0, VALUE a1)
{
    struct call_with_inspection_data cwi;
    VALUE argv[2];

    argv[0] = a0;
    argv[1] = a1;

    if (0) fprintf(stderr, "call_at: %s\n", rb_id2name(mid));

    cwi.debug_context = debug_context;
    cwi.context = context;
    cwi.id = mid;
    cwi.argc = argc;
    cwi.argv = &argv[0];
    return call_with_debug_inspector(&cwi);
}

static VALUE
call_at_line(VALUE context, debug_context_t *debug_context, VALUE file, VALUE line)
{
    save_current_position(debug_context, file, line);
    return call_at(context, debug_context, rb_intern("at_line"), 2, file, line);
}

static VALUE
call_at_tracing(VALUE context, debug_context_t *debug_context, VALUE file, VALUE line)
{
    return call_at(context, debug_context, rb_intern("at_tracing"), 2, file, line);
}

static VALUE
call_at_breakpoint(VALUE context, debug_context_t *debug_context, VALUE breakpoint)
{
    debug_context->stop_reason = CTX_STOP_BREAKPOINT;
    return call_at(context, debug_context, rb_intern("at_breakpoint"), 1, breakpoint, 0);
}

static VALUE
call_at_catchpoint(VALUE context, debug_context_t *debug_context, VALUE exp)
{
    return call_at(context, debug_context, rb_intern("at_catchpoint"), 1, exp, 0);
}

static void
call_at_line_check(VALUE binding, debug_context_t *debug_context, VALUE breakpoint, VALUE context, VALUE file, VALUE line)
{
    debug_context->stop_reason = CTX_STOP_STEP;

    /* check breakpoint expression */
    if (breakpoint != Qnil) {
	if (!check_breakpoint_expression(breakpoint, binding)) return;// TODO
	if (!check_breakpoint_hit_condition(breakpoint)) return;// TODO

	if (breakpoint != debug_context->breakpoint) {
	    call_at_breakpoint(context, debug_context, breakpoint);
	}
	else {
	    debug_context->breakpoint = Qnil;
	}
    }

    reset_stepping_stop_points(debug_context);
    call_at_line(context, debug_context, file, line);
}

static void
line_tracepoint(VALUE tpval, void *data)
{
    int moved = 0; /* TODO */
    VALUE breakpoint = Qnil;
    VALUE file, line;
    TRACE_SETUP;

    TRACE_COMMON();

    CTX_FL_SET(debug_context, CTX_FL_STEPPED);

    file = rb_tracearg_path(trace_arg);
    line = rb_tracearg_lineno(trace_arg);

    if(RTEST(tracing) || CTX_FL_TEST(debug_context, CTX_FL_TRACING)) {
	call_at_tracing(context, debug_context, file, line);
    }

    if (debug_context->dest_frame == -1 ||
	dc_stack_size(debug_context) == debug_context->dest_frame) {
	if (moved || !CTX_FL_TEST(debug_context, CTX_FL_FORCE_MOVE)) debug_context->stop_next--;
	if (debug_context->stop_next < 0)                            debug_context->stop_next = -1;

	if (moved || (CTX_FL_TEST(debug_context, CTX_FL_STEPPED) &&
		     !CTX_FL_TEST(debug_context, CTX_FL_FORCE_MOVE))) {
	    debug_context->stop_line--;
	    CTX_FL_UNSET(debug_context, CTX_FL_STEPPED);
	}
    }
    else if (dc_stack_size(debug_context) < debug_context->dest_frame) {
	debug_context->stop_next = 0;
    }

    if (0) fprintf(stderr, "stop_next: %d, stop_line: %d\n",
		   debug_context->stop_next,
		   debug_context->stop_line
		   );

    if (debug_context->stop_next == 0 ||
	debug_context->stop_line == 0 ||
	(breakpoint = check_breakpoints_by_pos(debug_context, file, line)) != Qnil) {
	call_at_line_check(rb_tracearg_binding(trace_arg), debug_context, breakpoint, context, file, line);
    }

    trace_cleanup(debug_context);
}

static void
call_tracepoint(VALUE tpval, void *data)
{
    VALUE breakpoint;
    VALUE klass, mid, self;
    TRACE_SETUP;

    debug_context->calced_stack_size ++;
    if (debug_debugger_stack_size && debug_debugger) {
	fprintf(stderr, "[debug:stacksize] %d (add)\n", debug_context->calced_stack_size);
    }

    TRACE_COMMON();

    klass = rb_tracearg_defined_class(trace_arg);
    mid = rb_tracearg_method_id(trace_arg);
    self = rb_tracearg_self(trace_arg);

    breakpoint = check_breakpoints_by_method(debug_context, klass, mid, self);

    if (breakpoint != Qnil) {
	VALUE binding = rb_tracearg_binding(trace_arg);

	if (!check_breakpoint_expression(breakpoint, binding)) return;
	if (!check_breakpoint_hit_condition(breakpoint)) return;
	if (breakpoint != debug_context->breakpoint) {
	    call_at_breakpoint(context, debug_context, breakpoint);
	}
	else {
	    debug_context->breakpoint = Qnil;
	}
	call_at_line(context, debug_context, rb_tracearg_path(trace_arg), rb_tracearg_lineno(trace_arg));
    }

    trace_cleanup(debug_context);
}

static void
return_tracepoint(VALUE tpval, void *data)
{
    TRACE_SETUP;

    debug_context->calced_stack_size --;

    TRACE_COMMON();

    if (debug_context->calced_stack_size + 1 == debug_context->stop_frame) {
	debug_context->stop_next = 1;
	debug_context->stop_frame = 0;
	/* NOTE: can't use call_at_line function here to trigger a debugger event.
                 this can lead to segfault. We should only unroll the stack on this event.
	 */
    }

    CTX_FL_SET(debug_context, CTX_FL_ENABLE_BKPT);

    trace_cleanup(debug_context);
}


static void
misc_call_tracepoint(VALUE tpval, void *data)
{
    TRACE_SETUP;
    debug_context->calced_stack_size ++;
    if (debug_debugger_stack_size && debug_debugger) {
	fprintf(stderr, "[debug:stacksize] %d (add)\n", debug_context->calced_stack_size);
    }
}

static void
misc_return_tracepoint(VALUE tpval, void *data)
{
    TRACE_SETUP;

    debug_context->calced_stack_size --;

    if (debug_debugger_stack_size && debug_debugger) {
	fprintf(stderr, "[debug:stacksize] %d (dec)\n", debug_context->calced_stack_size);
    }
    CTX_FL_SET(debug_context, CTX_FL_ENABLE_BKPT);
}

static void
raise_tracepoint(VALUE tpval, void *data)
{
    VALUE ancestors;
    VALUE expn_class, aclass;
    VALUE binding;
    VALUE err = rb_errinfo();
    int i;
    TRACE_SETUP;

    TRACE_COMMON();

    if (post_mortem == Qtrue) {
	binding = rb_tracearg_binding(trace_arg);
	rb_ivar_set(rb_errinfo(), rb_intern("@__debug_file"), rb_tracearg_path(trace_arg));
	rb_ivar_set(rb_errinfo(), rb_intern("@__debug_line"), rb_tracearg_lineno(trace_arg));
	rb_ivar_set(rb_errinfo(), rb_intern("@__debug_binding"), rb_tracearg_binding(trace_arg));
	rb_ivar_set(rb_errinfo(), rb_intern("@__debug_context"), debug_context_dup(debug_context, rb_tracearg_self(trace_arg)));
    }

    expn_class = rb_obj_class(err);

    if (rdebug_catchpoints == Qnil ||
	(dc_stack_size(debug_context) == 0) ||
	CTX_FL_TEST(debug_context, CTX_FL_CATCHING) ||
	(RHASH_TBL(rdebug_catchpoints)->num_entries) == 0) {
	goto cleanup;
    }

    ancestors = rb_mod_ancestors(expn_class);

    for (i = 0; i < RARRAY_LEN(ancestors); i++) {
	VALUE mod_name;
	VALUE hit_count;

	aclass    = rb_ary_entry(ancestors, i);
	mod_name  = rb_mod_name(aclass);
	hit_count = rb_hash_aref(rdebug_catchpoints, mod_name);

	if (hit_count != Qnil) {
	    /* increment exception */
	    rb_hash_aset(rdebug_catchpoints, mod_name, INT2FIX(FIX2INT(hit_count) + 1));
	    call_at_catchpoint(context, debug_context, err);
	    break;
	}
    }

  cleanup:
    trace_cleanup(debug_context);
}


static void
register_debug_trace_points(void)
{
    int i;
    VALUE traces = tracepoints;

    if (NIL_P(traces)) {
	traces = rb_ary_new();

	rb_ary_push(traces, rb_tracepoint_new(Qnil, RUBY_EVENT_LINE, line_tracepoint, 0));
	rb_ary_push(traces, rb_tracepoint_new(Qnil, RUBY_EVENT_CALL, call_tracepoint, 0));
	rb_ary_push(traces, rb_tracepoint_new(Qnil, RUBY_EVENT_RAISE, raise_tracepoint, 0));
	rb_ary_push(traces, rb_tracepoint_new(Qnil, RUBY_EVENT_RETURN | RUBY_EVENT_END, return_tracepoint, 0));
	rb_ary_push(traces, rb_tracepoint_new(Qnil, RUBY_EVENT_C_CALL | RUBY_EVENT_B_CALL, misc_call_tracepoint, 0));
	rb_ary_push(traces, rb_tracepoint_new(Qnil, RUBY_EVENT_C_RETURN | RUBY_EVENT_B_RETURN, misc_return_tracepoint, 0));
	tracepoints = traces;
    }

    for (i=0; i<RARRAY_LEN(traces); i++) {
	rb_tracepoint_enable(rb_ary_entry(traces, i));
    }
}

static void
clear_debug_trace_points(void)
{
    int i;

    for (i=0; i<RARRAY_LEN(tracepoints); i++) {
	rb_tracepoint_disable(rb_ary_entry(tracepoints, i));
    }
}

static VALUE
debug_stop_i(VALUE self)
{
    if (IS_STARTED) {
	debug_stop(self);
    }
    return Qnil;
}

/*
 *   call-seq:
 *      Debugger.start_ -> bool
 *      Debugger.start_ { ... } -> bool
 *
 *   This method is internal and activates the debugger. Use
 *   Debugger.start (from <tt>lib/ruby-debug-base.rb</tt>) instead.
 *
 *   The return value is the value of !Debugger.started? <i>before</i>
 *   issuing the +start+; That is, +true+ is returned, unless debugger
 *   was previously started.

 *   If a block is given, it starts debugger and yields to block. When
 *   the block is finished executing it stops the debugger with
 *   Debugger.stop method. Inside the block you will probably want to
 *   have a call to Debugger.debugger. For example:
 *     Debugger.start{debugger; foo}  # Stop inside of foo
 * 
 *   Also, ruby-debug only allows
 *   one invocation of debugger at a time; nested Debugger.start's
 *   have no effect and you can't use this inside the debugger itself.
 *
 *   <i>Note that if you want to completely remove the debugger hook,
 *   you must call Debugger.stop as many times as you called
 *   Debugger.start method.</i>
 */
static VALUE
debug_start(VALUE self)
{
    VALUE result;
    start_count++;

    if(IS_STARTED) {
	result = Qfalse;
    }
    else {
	locker             = Qnil;
	rdebug_breakpoints = rb_ary_new();
	rdebug_catchpoints = rb_hash_new();
	rdebug_threads_tbl = threads_table_create();

	register_debug_trace_points();
	debug_context_create(rb_thread_current());
	result = Qtrue;
    }

    if(rb_block_given_p())
      rb_ensure(rb_yield, self, debug_stop_i, self);

    return result;
}

/*
 *   call-seq:
 *      Debugger.stop -> bool
 *
 *   This method disables the debugger. It returns +true+ if the debugger is disabled,
 *   otherwise it returns +false+.
 *
 *   <i>Note that if you want to complete remove the debugger hook,
 *   you must call Debugger.stop as many times as you called
 *   Debugger.start method.</i>
 */
static VALUE
debug_stop(VALUE self)
{
    debug_check_started();

    start_count--;
    if(start_count)
        return Qfalse;


    clear_debug_trace_points();

    locker             = Qnil;
    rdebug_breakpoints = Qnil;
    rdebug_threads_tbl = Qnil;

    return Qtrue;
}

static int
find_last_context_func(st_data_t key, st_data_t value, st_data_t *result_arg)
{
    debug_context_t *debug_context;
    VALUE *result = (VALUE*)result_arg;
    if(!value)
        return ST_CONTINUE;

    Data_Get_Struct((VALUE)value, debug_context_t, debug_context);
    if(debug_context->thnum == last_debugged_thnum)
    {
        *result = value;
        return ST_STOP;
    }
    return ST_CONTINUE;
}

/*
 *   call-seq:
 *      Debugger.last_interrupted -> context
 *
 *   Returns last debugged context.
 */
static VALUE
debug_last_interrupted(VALUE self)
{
    VALUE result = Qnil;
    threads_table_t *threads_table;

    debug_check_started();

    Data_Get_Struct(rdebug_threads_tbl, threads_table_t, threads_table);

    st_foreach(threads_table->tbl, find_last_context_func, (st_data_t)&result);
    return result;
}

/*
 *   call-seq:
 *      Debugger.current_context -> context
 *
 *   Returns current context.
 *   <i>Note:</i> Debugger.current_context.thread == Thread.current
 */
static VALUE
debug_current_context(VALUE self)
{
    VALUE thread, context;

    debug_check_started();

    thread = rb_thread_current();
    thread_context_lookup(thread, &context, NULL, 1);

    return context;
}

/*
 *   call-seq:
 *      Debugger.thread_context(thread) -> context
 *
 *   Returns context of the thread passed as an argument.
 */
static VALUE
debug_thread_context(VALUE self, VALUE thread)
{
    VALUE context;

    debug_check_started();
    thread_context_lookup(thread, &context, NULL, 1);
    return context;
}

/*
 *   call-seq:
 *      Debugger.contexts -> array
 *
 *   Returns an array of all contexts.
 */
static VALUE
debug_contexts(VALUE self)
{
    volatile VALUE list;
    volatile VALUE new_list;
    VALUE thread, context;
    threads_table_t *threads_table;
    debug_context_t *debug_context;
    int i;

    debug_check_started();

    new_list = rb_ary_new();
    list = rb_funcall(rb_cThread, rb_intern("list"), 0);

    for (i = 0; i < RARRAY_LEN(list); i++) {
	thread = rb_ary_entry(list, i);
	thread_context_lookup(thread, &context, NULL, 1);
	rb_ary_push(new_list, context);
    }

    threads_table_clear(rdebug_threads_tbl);
    Data_Get_Struct(rdebug_threads_tbl, threads_table_t, threads_table);

    for (i = 0; i < RARRAY_LEN(new_list); i++) {
	context = rb_ary_entry(new_list, i);
	Data_Get_Struct(context, debug_context_t, debug_context);
	st_insert(threads_table->tbl, debug_context->thread_id, context);
    }

    return new_list;
}

/*
 *   call-seq:
 *      Debugger.suspend -> Debugger
 *
 *   Suspends all contexts.
 */
static VALUE
debug_suspend(VALUE self)
{
    VALUE current, context;
    VALUE context_list;
    debug_context_t *debug_context;
    int i;

    debug_check_started();

    context_list = debug_contexts(self);
    thread_context_lookup(rb_thread_current(), &current, NULL, 1);

    for(i = 0; i < RARRAY_LEN(context_list); i++)
    {
        context = rb_ary_entry(context_list, i);
        if(current == context)
            continue;
        Data_Get_Struct(context, debug_context_t, debug_context);
        context_suspend_0(debug_context);
    }

    return self;
}

/*
 *   call-seq:
 *      Debugger.resume -> Debugger
 *
 *   Resumes all contexts.
 */
static VALUE
debug_resume(VALUE self)
{
    VALUE current, context;
    VALUE context_list;
    debug_context_t *debug_context;
    int i;

    debug_check_started();

    context_list = debug_contexts(self);

    thread_context_lookup(rb_thread_current(), &current, NULL, 1);
    for(i = 0; i < RARRAY_LEN(context_list); i++)
    {
        context = rb_ary_entry(context_list, i);
        if(current == context)
            continue;
        Data_Get_Struct(context, debug_context_t, debug_context);
        context_resume_0(debug_context);
    }

    rb_thread_schedule();

    return self;
}

/*
 *   call-seq:
 *      Debugger.tracing -> bool
 *
 *   Returns +true+ if the global tracing is activated.
 */
static VALUE
debug_tracing(VALUE self)
{
    return tracing;
}

/*
 *   call-seq:
 *      Debugger.tracing = bool
 *
 *   Sets the global tracing flag.
 */
static VALUE
debug_set_tracing(VALUE self, VALUE value)
{
    tracing = RTEST(value) ? Qtrue : Qfalse;
    return value;
}

/*
 *   call-seq:
 *      Debugger.post_mortem? -> bool
 *
 *   Returns +true+ if post-moterm debugging is enabled.
 */
static VALUE
debug_post_mortem(VALUE self)
{
    return post_mortem;
}

/*
 *   call-seq:
 *      Debugger.post_mortem = bool
 *
 *   Sets post-moterm flag.
 *   FOR INTERNAL USE ONLY.
 */
static VALUE
debug_set_post_mortem(VALUE self, VALUE value)
{
    debug_check_started();

    post_mortem = RTEST(value) ? Qtrue : Qfalse;
    return value;
}

/*
 *   call-seq:
 *      Debugger.track_fame_args? -> bool
 *
 *   Returns +true+ if the debugger track frame argument values on calls.
 */
static VALUE
debug_track_frame_args(VALUE self)
{
    return track_frame_args;
}

/*
 *   call-seq:
 *      Debugger.track_frame_args = bool
 *
 *   Setting to +true+ will make the debugger save argument info on calls.
 */
static VALUE
debug_set_track_frame_args(VALUE self, VALUE value)
{
    track_frame_args = RTEST(value) ? Qtrue : Qfalse;
    return value;
}

/*
 *   call-seq:
 *      Debugger.keep_frame_binding? -> bool
 *
 *   Returns +true+ if the debugger will collect frame bindings.
 */
static VALUE
debug_keep_frame_binding(VALUE self)
{
    return keep_frame_binding;
}

/*
 *   call-seq:
 *      Debugger.keep_frame_binding = bool
 *
 *   Setting to +true+ will make the debugger create frame bindings.
 */
static VALUE
debug_set_keep_frame_binding(VALUE self, VALUE value)
{
    keep_frame_binding = RTEST(value) ? Qtrue : Qfalse;
    return value;
}

/* :nodoc: */
static VALUE
debug_debug(VALUE self)
{
    return debug_debugger;
}

/* :nodoc: */
static VALUE
debug_set_debug(VALUE self, VALUE value)
{
    debug_debugger = RTEST(value) ? Qtrue : Qfalse;
    return value;
}

/* :nodoc: */
static VALUE
debug_thread_inherited(VALUE klass)
{
    rb_raise(rb_eRuntimeError, "Can't inherit Debugger::DebugThread class");
}

/*
 *   call-seq:
 *      Debugger.debug_load(file, stop = false, increment_start = false) -> nil
 *
 *   Same as Kernel#load but resets current context's frames.
 *   +stop+ parameter forces the debugger to stop at the first line of code in the +file+
 *   +increment_start+ determines if start_count should be incremented. When
 *    control threads are used, they have to be set up before loading the
 *    debugger; so here +increment_start+ will be false.    
 *   FOR INTERNAL USE ONLY.
 */
static VALUE
debug_debug_load(int argc, VALUE *argv, VALUE self)
{
    VALUE file, stop, context, increment_start;
    debug_context_t *debug_context;
    int state = 0;
    
    if (rb_scan_args(argc, argv, "12", &file, &stop, &increment_start) == 1) {
	stop = Qfalse;
	increment_start = Qtrue;
    }

    debug_start(self);
    if (Qfalse == increment_start) start_count--;
    
    context = debug_current_context(self);
    Data_Get_Struct(context, debug_context_t, debug_context);

    /* debug_context->stack_size = 0; */

    if (RTEST(stop)) {
        debug_context->stop_next = 1;
    }
    /* Initializing $0 to the script's path */
    ruby_script(RSTRING_PTR(file));
    rb_load_protect(file, 0, &state);
    if (0 != state) 
    {
        VALUE errinfo = rb_errinfo();
        debug_suspend(self);
        reset_stepping_stop_points(debug_context);
        rb_set_errinfo(Qnil);
        return errinfo;
    }

    /* We should run all at_exit handler's in order to provide, 
     * for instance, a chance to run all defined test cases */
    rb_exec_end_proc();

    /* We could have issued a Debugger.stop inside the debug
       session. */
    if (start_count > 0)
        debug_stop(self);

    return Qnil;
}

static VALUE
set_current_skipped_status(VALUE status)
{
    VALUE context;
    debug_context_t *debug_context;

    context = debug_current_context(Qnil);
    Data_Get_Struct(context, debug_context_t, debug_context);
    if (status) {
	CTX_FL_SET(debug_context, CTX_FL_SKIPPED);
    }
    else {
        CTX_FL_UNSET(debug_context, CTX_FL_SKIPPED);
    }
    return Qnil;
}

/*
 *   call-seq:
 *      Debugger.skip { block } -> obj or nil
 *
 *   The code inside of the block is escaped from the debugger.
 */
static VALUE
debug_skip(VALUE self)
{
    if (!rb_block_given_p()) {
        rb_raise(rb_eArgError, "called without a block");
    }

    if (!IS_STARTED) {
	return rb_yield(Qnil);
    }
    set_current_skipped_status(Qtrue);
    return rb_ensure(rb_yield, Qnil, set_current_skipped_status, Qfalse);
}

static VALUE
debug_at_exit_c(VALUE proc)
{
    return rb_funcall(proc, rb_intern("call"), 0);
}

static void
debug_at_exit_i(VALUE proc)
{
    if(!IS_STARTED)
    {
        debug_at_exit_c(proc);
    }
    else
    {
        set_current_skipped_status(Qtrue);
        rb_ensure(debug_at_exit_c, proc, set_current_skipped_status, Qfalse);
    }
}

/*
 *   call-seq:
 *      Debugger.debug_at_exit { block } -> proc
 *
 *   Register <tt>at_exit</tt> hook which is escaped from the debugger.
 *   FOR INTERNAL USE ONLY.
 */
static VALUE
debug_at_exit(VALUE self)
{
    VALUE proc;
    if (!rb_block_given_p())
        rb_raise(rb_eArgError, "called without a block");
    proc = rb_block_proc();
    rb_set_end_proc(debug_at_exit_i, proc);
    return proc;
}

/*
 *   call-seq:
 *      context.step(steps, force = false)
 *
 *   Stops the current context after a number of +steps+ are made.
 *   +force+ parameter (if true) ensures that the cursor moves from the current line.
 */
static VALUE
context_stop_next(int argc, VALUE *argv, VALUE self)
{
    VALUE steps, force;
    debug_context_t *debug_context;

    debug_check_started();

    rb_scan_args(argc, argv, "11", &steps, &force);
    if (FIX2INT(steps) < 0) {
	rb_raise(rb_eRuntimeError, "Steps argument can't be negative.");
    }

    Data_Get_Struct(self, debug_context_t, debug_context);

    debug_context->stop_next = FIX2INT(steps);

    if (RTEST(force)) {
	CTX_FL_SET(debug_context, CTX_FL_FORCE_MOVE);
    }
    else {
	CTX_FL_UNSET(debug_context, CTX_FL_FORCE_MOVE);
    }

    return steps;
}

/*
 *   call-seq:
 *      context.step_over(steps, frame = nil, force = false)
 *
 *   Steps over a +steps+ number of times.
 *   Make step over operation on +frame+, by default the current frame.
 *   +force+ parameter (if true) ensures that the cursor moves from the current line.
 */
static VALUE
context_step_over(int argc, VALUE *argv, VALUE self)
{
    VALUE lines, frame, force;
    debug_context_t *debug_context;

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);

    if (dc_stack_size(debug_context) == 0) {
	rb_raise(rb_eRuntimeError, "No frames collected.");
    }

    rb_scan_args(argc, argv, "12", &lines, &frame, &force);
    debug_context->stop_line = FIX2INT(lines);
    CTX_FL_UNSET(debug_context, CTX_FL_STEPPED);

    if (frame == Qnil) {
	debug_context->dest_frame = dc_stack_size(debug_context);
    }
    else {
	if (FIX2INT(frame) < 0 && FIX2INT(frame) >= dc_stack_size(debug_context)) {
	    rb_raise(rb_eRuntimeError, "Destination frame is out of range.");
	    debug_context->dest_frame = dc_stack_size(debug_context) - FIX2INT(frame);
	}
    }

    if (RTEST(force)) {
	CTX_FL_SET(debug_context, CTX_FL_FORCE_MOVE);
    }
    else {
	CTX_FL_UNSET(debug_context, CTX_FL_FORCE_MOVE);
    }

    return Qnil;
}

/*
 *   call-seq:
 *      context.stop_frame(frame)
 *
 *   Stops when a frame with number +frame+ is activated. Implements +finish+ and +next+ commands.
 */
static VALUE
context_stop_frame(VALUE self, VALUE frame)
{
    debug_context_t *debug_context;

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);

    if (FIX2INT(frame) < 0 || FIX2INT(frame) >= dc_stack_size(debug_context)) {
	rb_raise(rb_eRuntimeError, "Stop frame is out of range.");
    }
    debug_context->stop_frame = dc_stack_size(debug_context) - FIX2INT(frame);

    return frame;
}

static int
check_frame_number(debug_context_t *debug_context, int frame)
{
    if (frame < 0 || frame >= dc_stack_size(debug_context)) {
	rb_raise(rb_eArgError, "Invalid frame number %d, stack (0...%d)",
		 frame, dc_stack_size(debug_context) - 1);
    }

    return frame;
}

static int 
optional_frame_position(int argc, VALUE *argv)
{
    VALUE level = INT2FIX(0);

    if ((argc > 1) || (argc < 0)) {
	rb_raise(rb_eArgError, "wrong number of arguments (%d for 0 or 1)", argc);
    }
    rb_scan_args(argc, argv, "01", &level);
    return FIX2INT(level);
}

/*
 *   call-seq:
 *      context.frame_args_info(frame_position=0) -> list 
        if track_frame_args or nil otherwise
 *
 *   Returns info saved about call arguments (if any saved).
 */
static VALUE
context_frame_args_info(int argc, VALUE *argv, VALUE self)
{
    int frame;
    debug_context_t *debug_context;
    VALUE binding;
    const char src[] = "method(__method__).parameters";

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);
    frame = check_frame_number(debug_context, optional_frame_position(argc, argv));

    binding = dc_inspected_frame_binding(debug_context, frame);
    return NIL_P(binding) ? rb_ary_new() : rb_funcall(binding, rb_intern("eval"), 1, rb_str_new2(src));
}

/*
 *   call-seq:
 *      context.frame_binding(frame_position=0) -> binding
 *
 *   Returns frame's binding.
 */
static VALUE
context_frame_binding(int argc, VALUE *argv, VALUE self)
{
    int frame;
    debug_context_t *debug_context;

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);
    frame = check_frame_number(debug_context, optional_frame_position(argc, argv));

    return dc_inspected_frame_binding(debug_context, frame);
}

/*
 *   call-seq:
 *      context.frame_method(frame_position=0) -> sym
 *
 *   Returns the sym of the called method.
 */
static VALUE
context_frame_id(int argc, VALUE *argv, VALUE self)
{
    int frame;
    debug_context_t *debug_context;
    VALUE loc;

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);
    frame = check_frame_number(debug_context, optional_frame_position(argc, argv));
    loc = dc_inspected_frame_location(debug_context, frame);

    return rb_str_intern(rb_funcall(loc, rb_intern("label"), 0));
}

/*
 *   call-seq:
 *      context.frame_line(frame_position) -> int
 *
 *   Returns the line number in the file.
 */
static VALUE
context_frame_line(int argc, VALUE *argv, VALUE self)
{
    int frame;
    debug_context_t *debug_context;
    VALUE loc;

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);
    frame = check_frame_number(debug_context, optional_frame_position(argc, argv));
    loc = dc_inspected_frame_location(debug_context, frame);

    return rb_funcall(loc, rb_intern("lineno"), 0);
}

/*
 *   call-seq:
 *      context.frame_file(frame_position) -> string
 *
 *   Returns the name of the file.
 */
static VALUE
context_frame_file(int argc, VALUE *argv, VALUE self)
{
    int frame;
    debug_context_t *debug_context;
    VALUE loc;

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);
    frame = check_frame_number(debug_context, optional_frame_position(argc, argv));
    loc = dc_inspected_frame_location(debug_context, frame);

    return rb_funcall(loc, rb_intern("absolute_path"), 0);
}

/*
 *   call-seq:
 *      context.frame_locals(frame) -> hash
 *
 *   Returns frame's local variables.
 */
static VALUE
context_frame_locals(int argc, VALUE *argv, VALUE self)
{
    int frame;
    debug_context_t *debug_context;
    VALUE binding;
    const char src[] = "local_variables.inject({}){|h, v| h[v] = eval(\"#{v}\"); h}";

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);
    frame = check_frame_number(debug_context, optional_frame_position(argc, argv));

    
    binding = dc_inspected_frame_binding(debug_context, frame);
    return NIL_P(binding) ? rb_hash_new() : rb_funcall(binding, rb_intern("eval"), 1, rb_str_new2(src));
}

/*
 *   call-seq:
 *      context.frame_args(frame_position=0) -> list
 *
 *   Returns frame's argument parameters
 */
static VALUE
context_frame_args(int argc, VALUE *argv, VALUE self)
{
    int frame;
    debug_context_t *debug_context;
    VALUE binding;
    const char src[] = "__method__ ? self.method(__method__).parameters.map{|(attr, mid)| mid} : []";

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);
    frame = check_frame_number(debug_context, optional_frame_position(argc, argv));

    binding = dc_inspected_frame_binding(debug_context, frame);
    return NIL_P(binding) ? rb_ary_new() : rb_funcall(binding, rb_intern("eval"), 1, rb_str_new2(src));
}

/*
 *   call-seq:
 *      context.frame_self(frame_postion=0) -> obj
 *
 *   Returns self object of the frame.
 */
static VALUE
context_frame_self(int argc, VALUE *argv, VALUE self)
{
    int frame;
    debug_context_t *debug_context;

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);
    frame = check_frame_number(debug_context, optional_frame_position(argc, argv));

    return dc_inspected_frame_self(debug_context, frame);
}

/*
 *   call-seq:
 *      context.frame_class(frame_position) -> obj
 *
 *   Returns the real class of the frame. 
 *   It could be different than context.frame_self(frame).class
 */
static VALUE
context_frame_class(int argc, VALUE *argv, VALUE self)
{
    int frame;
    debug_context_t *debug_context;
    
    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);
    frame = check_frame_number(debug_context, optional_frame_position(argc, argv));

    return dc_inspected_frame_class(debug_context, frame);
}

/*
 *   call-seq:
 *      context.stack_size-> int
 *
 *   Returns the size of the context stack.
 */
static VALUE
context_stack_size(VALUE self)
{
    debug_context_t *debug_context;

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);

    return INT2FIX(dc_stack_size(debug_context));
}

/*
 *   call-seq:
 *      context.thread -> thread
 *
 *   Returns a thread this context is associated with.
 */
static VALUE
context_thread(VALUE self)
{
    debug_context_t *debug_context;

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);

    return(id2ref(debug_context->thread_id));
}

/*
 *   call-seq:
 *      context.thnum -> int
 *
 *   Returns the context's number.
 */
static VALUE
context_thnum(VALUE self)
{
    debug_context_t *debug_context;

    debug_check_started();
    Data_Get_Struct(self, debug_context_t, debug_context);

    return INT2FIX(debug_context->thnum);
}

static void
context_suspend_0(debug_context_t *debug_context)
{
    VALUE status;

    status = rb_funcall(context_thread_0(debug_context), rb_intern("status"), 0);
    if(rb_str_cmp(status, rb_str_new2("run")) == 0) {
	CTX_FL_SET(debug_context, CTX_FL_WAS_RUNNING);
    }
    else if(rb_str_cmp(status, rb_str_new2("sleep")) == 0) {
	CTX_FL_UNSET(debug_context, CTX_FL_WAS_RUNNING);
    }
    else {
	return;
    }

    CTX_FL_SET(debug_context, CTX_FL_SUSPEND);
}

static void
context_resume_0(debug_context_t *debug_context)
{
    if(!CTX_FL_TEST(debug_context, CTX_FL_SUSPEND)) {
	return;
    }

    CTX_FL_UNSET(debug_context, CTX_FL_SUSPEND);

    if(CTX_FL_TEST(debug_context, CTX_FL_WAS_RUNNING)) {
	rb_thread_wakeup(context_thread_0(debug_context));
    }
}

/*
 *   call-seq:
 *      context.suspend -> nil
 *
 *   Suspends the thread when it is running.
 */
static VALUE
context_suspend(VALUE self)
{
    debug_context_t *debug_context;

    debug_check_started();

    Data_Get_Struct(self, debug_context_t, debug_context);

    if(CTX_FL_TEST(debug_context, CTX_FL_SUSPEND)) {
	rb_raise(rb_eRuntimeError, "Already suspended.");
    }
    context_suspend_0(debug_context);
    return Qnil;
}

/*
 *   call-seq:
 *      context.suspended? -> bool
 *
 *   Returns +true+ if the thread is suspended by debugger.
 */
static VALUE
context_is_suspended(VALUE self)
{
    debug_context_t *debug_context;

    debug_check_started();

    Data_Get_Struct(self, debug_context_t, debug_context);
    return CTX_FL_TEST(debug_context, CTX_FL_SUSPEND) ? Qtrue : Qfalse;
}

/*
 *   call-seq:
 *      context.resume -> nil
 *
 *   Resumes the thread from the suspended mode.
 */
static VALUE
context_resume(VALUE self)
{
    debug_context_t *debug_context;

    debug_check_started();

    Data_Get_Struct(self, debug_context_t, debug_context);
    if(!CTX_FL_TEST(debug_context, CTX_FL_SUSPEND)) {
	rb_raise(rb_eRuntimeError, "Thread is not suspended.");
    }
    context_resume_0(debug_context);
    return Qnil;
}

/*
 *   call-seq:
 *      context.tracing -> bool
 *
 *   Returns the tracing flag for the current context.
 */
static VALUE
context_tracing(VALUE self)
{
    debug_context_t *debug_context;

    debug_check_started();

    Data_Get_Struct(self, debug_context_t, debug_context);
    return CTX_FL_TEST(debug_context, CTX_FL_TRACING) ? Qtrue : Qfalse;
}

/*
 *   call-seq:
 *      context.tracing = bool
 *
 *   Controls the tracing for this context.
 */
static VALUE
context_set_tracing(VALUE self, VALUE value)
{
    debug_context_t *debug_context;

    debug_check_started();

    Data_Get_Struct(self, debug_context_t, debug_context);
    if (RTEST(value)) {
	CTX_FL_SET(debug_context, CTX_FL_TRACING);
    }
    else {
        CTX_FL_UNSET(debug_context, CTX_FL_TRACING);
    }
    return value;
}

/*
 *   call-seq:
 *      context.ignored? -> bool
 *
 *   Returns the ignore flag for the current context.
 */
static VALUE
context_ignored(VALUE self)
{
    debug_context_t *debug_context;

    debug_check_started();

    Data_Get_Struct(self, debug_context_t, debug_context);
    return CTX_FL_TEST(debug_context, CTX_FL_IGNORE) ? Qtrue : Qfalse;
}

/*
 *   call-seq:
 *      context.dead? -> bool
 *
 *   Returns +true+ if context doesn't represent a live context and is created
 *   during post-mortem exception handling.
 */
static VALUE
context_dead(VALUE self)
{
    debug_context_t *debug_context;

    debug_check_started();

    Data_Get_Struct(self, debug_context_t, debug_context);
    return CTX_FL_TEST(debug_context, CTX_FL_DEAD) ? Qtrue : Qfalse;
}

/*
 *   call-seq:
 *      context.stop_reason -> sym
 *   
 *   Returns the reason for the stop. It maybe of the following values:
 *   :initial, :step, :breakpoint, :catchpoint, :post-mortem
 */
static VALUE
context_stop_reason(VALUE self)
{
    debug_context_t *debug_context;
    const char * sym_name;

    debug_check_started();

    Data_Get_Struct(self, debug_context_t, debug_context);

    switch(debug_context->stop_reason) {
      case CTX_STOP_STEP:
	sym_name = "step";
	break;
      case CTX_STOP_BREAKPOINT:
	sym_name = "breakpoint";
	break;
      case CTX_STOP_CATCHPOINT:
	sym_name = "catchpoint";
	break;
      case CTX_STOP_NONE:
      default:
	sym_name = "none";
    }

    if(CTX_FL_TEST(debug_context, CTX_FL_DEAD)) {
	sym_name = "post-mortem";
    }

    return ID2SYM(rb_intern(sym_name));
}

/*
* call-seq:
* context.jump(line, file) -> bool
*
* Returns +true+ if jump to +line+ in filename +file+ was successful.
*/
static VALUE
context_jump(VALUE self, VALUE line, VALUE file)
{
    return INT2FIX(1);

#if 0
    debug_context_t *debug_context;
    debug_frame_t *debug_frame;
    int i;
    rb_thread_t *th;
    rb_control_frame_t *cfp;
    rb_control_frame_t *cfp_end;
    rb_control_frame_t *cfp_start = NULL;

    debug_check_started();

    Data_Get_Struct(self, debug_context_t, debug_context);
    GetThreadPtr(context_thread_0(debug_context), th);
    debug_frame = get_top_frame(debug_context);
    if (debug_frame == NULL)
      rb_raise(rb_eRuntimeError, "No frames collected.");

    line = FIX2INT(line);

    /* find topmost frame of the debugged code */
    cfp = th->cfp;
    cfp_end = RUBY_VM_END_CONTROL_FRAME(th);
    while (RUBY_VM_VALID_CONTROL_FRAME_P(cfp, cfp_end))
      {
	  if (cfp->pc == debug_frame->info.runtime.last_pc)
	    {
		cfp_start = cfp;
		if ((cfp->pc - cfp->iseq->iseq_encoded) >= (cfp->iseq->iseq_size - 1))
		  return(INT2FIX(1)); /* no space for opt_call_c_function hijack */
		break;
	    }
	  cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
      }
    if (cfp_start == NULL)
      return(INT2FIX(2)); /* couldn't find frame; should never happen */

    /* find target frame to jump to */
    while (RUBY_VM_VALID_CONTROL_FRAME_P(cfp, cfp_end))
      {
	  if ((cfp->iseq != NULL) && (rb_str_cmp(file, cfp->iseq->filename) == 0))
	    {
		for (i = 0; i < cfp->iseq->insn_info_size; i++)
		  {
		      if (cfp->iseq->insn_info_table[i].line_no != line)
			continue;

		      /* hijack the currently running code so that we can change the frame PC */
		      debug_context->saved_jump_ins[0] = cfp_start->pc[0];
		      debug_context->saved_jump_ins[1] = cfp_start->pc[1];
		      cfp_start->pc[0] = opt_call_c_function;
		      cfp_start->pc[1] = (VALUE)do_jump;

		      debug_context->jump_cfp = cfp;
		      debug_context->jump_pc =
			cfp->iseq->iseq_encoded + cfp->iseq->insn_info_table[i].position;

		      return(INT2FIX(0)); /* success */
		  }
	    }

	  cfp = RUBY_VM_PREVIOUS_CONTROL_FRAME(cfp);
      }

    return(INT2FIX(3)); /* couldn't find a line and file frame match */
#endif
}

/*
* call-seq:
* context.break -> bool
*
* Returns +true+ if context is currently running and set flag to break it at next line
*/
static VALUE
context_pause(VALUE self)
{
    debug_context_t *debug_context;

    debug_check_started();

    Data_Get_Struct(self, debug_context_t, debug_context);

    if (CTX_FL_TEST(debug_context, CTX_FL_DEAD)) {
	return(Qfalse);
    }

    if (debug_context->thread_id == rb_thread_current()) {
	return(Qfalse);
    }

    debug_context->thread_pause = 1;
    return(Qtrue);
}

/*
 *   Document-class: Context
 *
 *   == Summary
 *
 *   Debugger keeps a single instance of this class for each Ruby thread.
 */
static void
Init_context(void)
{
    cContext = rb_define_class_under(mDebugger, "Context", rb_cObject);
    rb_define_method(cContext, "stop_next=", context_stop_next, -1);
    rb_define_method(cContext, "step", context_stop_next, -1);
    rb_define_method(cContext, "step_over", context_step_over, -1);
    rb_define_method(cContext, "stop_frame=", context_stop_frame, 1);
    rb_define_method(cContext, "thread", context_thread, 0);
    rb_define_method(cContext, "thnum", context_thnum, 0);
    rb_define_method(cContext, "stop_reason", context_stop_reason, 0);
    rb_define_method(cContext, "suspend", context_suspend, 0);
    rb_define_method(cContext, "suspended?", context_is_suspended, 0);
    rb_define_method(cContext, "resume", context_resume, 0);
    rb_define_method(cContext, "tracing", context_tracing, 0);
    rb_define_method(cContext, "tracing=", context_set_tracing, 1);
    rb_define_method(cContext, "ignored?", context_ignored, 0);
    rb_define_method(cContext, "frame_args", context_frame_args, -1);
    rb_define_method(cContext, "frame_args_info", context_frame_args_info, -1);
    rb_define_method(cContext, "frame_binding", context_frame_binding, -1);
    rb_define_method(cContext, "frame_class", context_frame_class, -1);
    rb_define_method(cContext, "frame_file", context_frame_file, -1);
    rb_define_method(cContext, "frame_id", context_frame_id, -1);
    rb_define_method(cContext, "frame_line", context_frame_line, -1);
    rb_define_method(cContext, "frame_locals", context_frame_locals, -1);
    rb_define_method(cContext, "frame_method", context_frame_id, -1);
    rb_define_method(cContext, "frame_self", context_frame_self, -1);
    rb_define_method(cContext, "stack_size", context_stack_size, 0);
    rb_define_method(cContext, "dead?", context_dead, 0);
    rb_define_method(cContext, "breakpoint", context_breakpoint, 0);          /* in breakpoint.c */
    rb_define_method(cContext, "set_breakpoint", context_set_breakpoint, -1); /* in breakpoint.c */
    rb_define_method(cContext, "jump", context_jump, 2);
    rb_define_method(cContext, "pause", context_pause, 0);
}

/*
 *   call-seq:
 *      Debugger.breakpoints -> array
 *
 *   Returns an array of breakpoints.
 */
static VALUE
debug_breakpoints(VALUE self)
{
    debug_check_started();

    return rdebug_breakpoints;
}

/*
 *   call-seq:
 *      Debugger.add_breakpoint(source, pos, condition = nil) -> breakpoint
 *
 *   Adds a new breakpoint.
 *   <i>source</i> is a name of a file or a class.
 *   <i>pos</i> is a line number or a method name if <i>source</i> is a class name.
 *   <i>condition</i> is a string which is evaluated to +true+ when this breakpoint
 *   is activated.
 */
static VALUE
debug_add_breakpoint(int argc, VALUE *argv, VALUE self)
{
    VALUE result;

    debug_check_started();

    result = create_breakpoint_from_args(argc, argv, ++bkp_count);
    rb_ary_push(rdebug_breakpoints, result);
    return result;
}

/*
 *   Document-class: Debugger
 *
 *   == Summary
 *
 *   This is a singleton class allows controlling the debugger. Use it to start/stop debugger,
 *   set/remove breakpoints, etc.
 */
void
Init_ruby_debug(void)
{
    mDebugger = rb_define_module("Debugger");
    rb_define_const(mDebugger, "VERSION", rb_str_new2(DEBUG_VERSION));
    rb_define_module_function(mDebugger, "start_", debug_start, 0);
    rb_define_module_function(mDebugger, "stop", debug_stop, 0);
    rb_define_module_function(mDebugger, "started?", debug_is_started, 0);
    rb_define_module_function(mDebugger, "breakpoints", debug_breakpoints, 0);
    rb_define_module_function(mDebugger, "add_breakpoint", debug_add_breakpoint, -1);
    rb_define_module_function(mDebugger, "remove_breakpoint", rdebug_remove_breakpoint, 1); /* in breakpoint.c */
    rb_define_module_function(mDebugger, "add_catchpoint", rdebug_add_catchpoint, 1);       /* in breakpoint.c */
    rb_define_module_function(mDebugger, "catchpoints", debug_catchpoints, 0);              /* in breakpoint.c */
    rb_define_module_function(mDebugger, "last_context", debug_last_interrupted, 0);
    rb_define_module_function(mDebugger, "contexts", debug_contexts, 0);
    rb_define_module_function(mDebugger, "current_context", debug_current_context, 0);
    rb_define_module_function(mDebugger, "thread_context", debug_thread_context, 1);
    rb_define_module_function(mDebugger, "suspend", debug_suspend, 0);
    rb_define_module_function(mDebugger, "resume", debug_resume, 0);
    rb_define_module_function(mDebugger, "tracing", debug_tracing, 0);
    rb_define_module_function(mDebugger, "tracing=", debug_set_tracing, 1);
    rb_define_module_function(mDebugger, "debug_load", debug_debug_load, -1);
    rb_define_module_function(mDebugger, "skip", debug_skip, 0);
    rb_define_module_function(mDebugger, "debug_at_exit", debug_at_exit, 0);
    rb_define_module_function(mDebugger, "post_mortem?", debug_post_mortem, 0);
    rb_define_module_function(mDebugger, "post_mortem=", debug_set_post_mortem, 1);
    rb_define_module_function(mDebugger, "keep_frame_binding?", debug_keep_frame_binding, 0);
    rb_define_module_function(mDebugger, "keep_frame_binding=", debug_set_keep_frame_binding, 1);
    rb_define_module_function(mDebugger, "track_frame_args?", debug_track_frame_args, 0);
    rb_define_module_function(mDebugger, "track_frame_args=", debug_set_track_frame_args, 1);
    rb_define_module_function(mDebugger, "debug", debug_debug, 0);
    rb_define_module_function(mDebugger, "debug=", debug_set_debug, 1);

    cThreadsTable = rb_define_class_under(mDebugger, "ThreadsTable", rb_cObject);

    cDebugThread  = rb_define_class_under(mDebugger, "DebugThread", rb_cThread);
    rb_define_singleton_method(cDebugThread, "inherited",
			       debug_thread_inherited, 1);

    Init_context();
    Init_breakpoint();

    rb_global_variable(&last_context);
    rb_global_variable(&last_thread);
    rb_global_variable(&locker);
    rb_global_variable(&rdebug_breakpoints);
    rb_global_variable(&rdebug_catchpoints);
    rb_global_variable(&rdebug_threads_tbl);
    rb_global_variable(&tracepoints);
}
