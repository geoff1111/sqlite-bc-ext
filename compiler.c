#include "compiler.h"
#include "embedded_libraries.h"
#include <inttypes.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SC_UNUSED(x) ((void)(x))

#ifdef SC_TESTING
static size_t sc_test_alloc_count_value;
static size_t sc_test_alloc_fail_at_value;

void sc_test_alloc_reset(void)
{
  sc_test_alloc_count_value = 0u;
  sc_test_alloc_fail_at_value = 0u;
}

void sc_test_alloc_fail_at(size_t allocation_number)
{
  sc_test_alloc_count_value = 0u;
  sc_test_alloc_fail_at_value = allocation_number;
}

size_t sc_test_alloc_count(void)
{
  return sc_test_alloc_count_value;
}

static bool sc_test_should_fail_alloc(void)
{
  ++sc_test_alloc_count_value;
  return sc_test_alloc_fail_at_value != 0u
      && sc_test_alloc_count_value == sc_test_alloc_fail_at_value;
}

static void *sc_test_malloc(size_t n)
{
  return sc_test_should_fail_alloc() ? NULL : malloc(n);
}

static void *sc_test_realloc(void *p, size_t n)
{
  return sc_test_should_fail_alloc() ? NULL : realloc(p, n);
}

#define malloc(n) sc_test_malloc(n)
#define realloc(p, n) sc_test_realloc((p), (n))
#endif


bool
sc_numeric_prefix_valid (char prefix)
{
  unsigned char c = (unsigned char) prefix;
  if ((c >= (unsigned char) 'A' && c <= (unsigned char) 'Z')
      || (c >= (unsigned char) 'a' && c <= (unsigned char) 'z'))
    return true;
  switch (c)
  {
    case '#': case '@': case '%': case '!':
    case '?': case ':': case '^': case '~':
      return true;
    default:
      return false;
  }
}

static ScVmResult
vm_fail (ScVm *vm, ScVmResult code, const char *msg)
{
  ScFrame *fr;
  if (!vm)
    return code;
  vm->error.code = code;
  vm->error.symbol_id = SC_INVALID_SYMBOL;
  vm->error.ip = SC_INVALID_INDEX;
  fr = vm->frame_count ? &vm->frames[vm->frame_count - 1u] : NULL;
  if (fr && fr->proc)
  {
    vm->error.symbol_id = fr->proc->symbol_id;
    vm->error.ip = fr->ip ? fr->ip - 1u : 0u;
  }
  snprintf (vm->error.message, sizeof (vm->error.message), "%s",
            msg ? msg : "VM error");
  return code;
}

static void
sc_vm_clear_frames (ScVm *vm)
{
  uint16_t f;
  if (!vm)
    return;
  for (f = 0; f < vm->frame_count; ++f)
  {
    ScFrame *fr = &vm->frames[f];
    uint16_t j;
    for (j = 0; j < fr->sp; ++j)
      if (sc_vm_value_valid (vm, fr->values[j]))
        sc_vm_release_value (vm, fr->values[j]);
    free (fr->values);
    memset (fr, 0, sizeof (*fr));
  }
  vm->frame_count = 0;
}

bool
sc_vm_error_is_fatal (ScVmResult code)
{
  switch (code)
  {
  case SC_VM_OK:
  case SC_ERR_ARITHMETIC:
  case SC_ERR_DIVZERO:
  case SC_ERR_TYPE:
  case SC_ERR_STACK_OVERFLOW:
  case SC_ERR_FRAME_OVERFLOW:
  case SC_ERR_ARG_COUNT:
  case SC_ERR_NO_RETURN_VALUE:
  case SC_ERR_MAX_INSTRUCTIONS:
  case SC_ERR_INVALID_PREFIX:
#if SC_ENABLE_PERSISTENT_STATE
  case SC_ERR_STATE_NOT_ACTIVE:
  case SC_ERR_STATE_ALREADY_ACTIVE:
  case SC_ERR_STATE_READ_ONLY:
  case SC_ERR_STATE_RANGE:
  case SC_ERR_STATE_INVALID_COUNT:
#endif
    return false;
  default:
    return true;
  }
}

bool
sc_compile_error_is_fatal (ScCompileResult code)
{
  return code == SC_ERR_COMPILE_NO_MEM || code == SC_ERR_COMPILE_INTERNAL;
}

ScVmHealth
sc_vm_health (const ScVm *vm)
{
  return vm ? vm->health : SC_VM_HEALTH_FATAL_ERROR;
}

bool
sc_vm_is_usable (const ScVm *vm)
{
  return vm && vm->health != SC_VM_HEALTH_FATAL_ERROR;
}

static ScVmResult
sc_vm_finish_error (ScVm *vm, ScVmResult rc)
{
  ScVmError saved;
  if (!vm)
    return rc;
  saved = vm->error;
  sc_vm_clear_frames (vm);
  if (vm->call_result_set && sc_vm_value_valid (vm, vm->call_result))
    sc_vm_release_value (vm, vm->call_result);
  vm->call_args = NULL;
  vm->call_argc = 0;
  vm->call_result = SC_INVALID_VALUE;
  vm->call_result_set = false;
  vm->instruction_count = 0;
#if SC_ENABLE_PERSISTENT_STATE
  sc_vm_state_rollback (vm);
#endif
  bcl_gc ();
  vm->error = saved;
  vm->health = sc_vm_error_is_fatal (rc)
    ? SC_VM_HEALTH_FATAL_ERROR : SC_VM_HEALTH_RECOVERABLE_ERROR;
  return rc;
}

ScVmResult
sc_vm_reset_after_error (ScVm *vm)
{
  if (!vm)
    return SC_ERR_INTERNAL;
  if (vm->health == SC_VM_HEALTH_FATAL_ERROR)
    return vm->error.code != SC_VM_OK ? vm->error.code : SC_ERR_INTERNAL;
  sc_vm_clear_frames (vm);
  if (vm->call_result_set && sc_vm_value_valid (vm, vm->call_result))
    sc_vm_release_value (vm, vm->call_result);
  vm->call_args = NULL;
  vm->call_argc = 0;
  vm->call_result = SC_INVALID_VALUE;
  vm->call_result_set = false;
  vm->instruction_count = 0;
#if SC_ENABLE_PERSISTENT_STATE
  sc_vm_state_rollback (vm);
#endif
  bcl_gc ();
  vm->health = SC_VM_HEALTH_READY;
  sc_vm_clear_error (vm);
  return SC_VM_OK;
}

static bool
grow_array (void **p, size_t elem, uint32_t *cap, uint32_t need, uint32_t max)
{
  uint32_t nc = *cap ? *cap : 8u;
  void *np;
  if (need > max)
    return false;
  while (nc < need)
  {
    if (nc > max / 2u)
    {
      nc = max;
      break;
    }
    nc *= 2u;
  }
  if (nc < need)
    return false;
  np = realloc (*p, elem * nc);
  if (!np)
    return false;
  *p = np;
  *cap = nc;
  return true;
}

void
sc_security_limits_default (ScSecurityLimits *l)
{
  if (!l)
    return;
  l->max_source_len = SC_MAX_SOURCE_LEN;
  l->max_bytecode_bytes = SC_MAX_BYTECODE_BYTES;
  l->max_procs = SC_MAX_PROCS;
  l->max_symbols = SC_MAX_SYMBOLS;
  l->max_constants = SC_MAX_CONSTANTS;
  l->max_args = SC_MAX_ARGS;
  l->max_locals = SC_MAX_LOCALS;
  l->max_stack = SC_MAX_STACK;
  l->max_frames = SC_MAX_FRAMES;
  l->max_loop_depth = SC_MAX_LOOP_DEPTH;
  l->max_parse_depth = SC_MAX_PARSE_DEPTH;
  l->max_instructions = SC_MAX_INSTRUCTIONS;
}

uint32_t
sc_hash_bytes (const char *p, size_t len)
{
  uint32_t h = 2166136261u;
  size_t i;
  for (i = 0; i < len; ++i)
  {
    h ^= (unsigned char) p[i];
    h *= 16777619u;
  }
  return h ? h : 1u;
}

bool
sc_slice_equal_cstr (ScSlice s, const char *z)
{
  size_t n = strlen (z);
  return s.len == n && memcmp (s.p, z, n) == 0;
}

ScVmResult
sc_symbol_table_init (ScSymbolTable *t, uint32_t buckets)
{
  uint32_t i;
  if (!t || buckets == 0)
    return SC_ERR_INTERNAL;
  memset (t, 0, sizeof (*t));
  t->buckets = malloc (sizeof (uint32_t) * buckets);
  if (!t->buckets)
    return SC_ERR_NO_MEM;
  for (i = 0; i < buckets; ++i)
    t->buckets[i] = SC_INVALID_INDEX;
  t->bucket_count = buckets;
  return SC_VM_OK;
}

void
sc_symbol_table_destroy (ScSymbolTable *t)
{
  uint32_t i;
  if (!t)
    return;
  for (i = 0; i < t->count; ++i)
    free (t->symbols[i].name);
  free (t->symbols);
  free (t->buckets);
  memset (t, 0, sizeof (*t));
}

ScVmResult
sc_symbol_lookup (const ScSymbolTable *t, ScSlice name, ScSymbolId *out)
{
  uint32_t h, i;
  if (!t || (!name.p && name.len != 0u) || !out || !t->bucket_count)
    return SC_ERR_INTERNAL;
  h = sc_hash_bytes (name.p, name.len);
  i = t->buckets[h % t->bucket_count];
  while (i != SC_INVALID_INDEX)
  {
    const ScSymbol *s = &t->symbols[i];
    if (s->hash == h && s->name_len == name.len
	&& memcmp (s->name, name.p, name.len) == 0)
    {
      *out = i;
      return SC_VM_OK;
    }
    i = s->next;
  }
  return SC_ERR_INVALID_SYMBOL;
}

ScVmResult
sc_symbol_add (ScSymbolTable *t, ScSlice name, uint16_t argc,
	       ScSymbolKind kind, ScSymbolId *out)
{
  ScSymbolId old;
  uint32_t h, b, id;
  char *copy;
  if (!t || !name.p || !out || name.len == 0 || name.len > SC_MAX_NAME_LEN)
    return SC_ERR_INVALID_SYMBOL;
  if (sc_symbol_lookup (t, name, &old) == SC_VM_OK)
    return SC_ERR_INVALID_SYMBOL;
  if (!grow_array
      ((void **) &t->symbols, sizeof (*t->symbols), &t->capacity,
       t->count + 1u, SC_MAX_SYMBOLS))
    return SC_ERR_NO_MEM;
  copy = malloc (name.len + 1u);
  if (!copy)
    return SC_ERR_NO_MEM;
  memcpy (copy, name.p, name.len);
  copy[name.len] = '\0';
  id = t->count++;
  h = sc_hash_bytes (name.p, name.len);
  b = h % t->bucket_count;
  memset (&t->symbols[id], 0, sizeof (t->symbols[id]));
  t->symbols[id].name = copy;
  t->symbols[id].name_len = (uint16_t) name.len;
  t->symbols[id].argc = argc;
  t->symbols[id].hash = h;
  t->symbols[id].kind = kind;
  t->symbols[id].next = t->buckets[b];
  t->buckets[b] = id;
  *out = id;
  return SC_VM_OK;
}

bool
sc_vm_value_valid (const ScVm *vm, ScValue v)
{
  return vm && v < vm->slot_count && vm->slots[v].refcnt != 0;
}

BclNumber
sc_vm_value_number (const ScVm *vm, ScValue v)
{
  BclNumber bad = { SIZE_MAX };
  return sc_vm_value_valid (vm, v) ? vm->slots[v].num : bad;
}


static int
parse_int64_decimal (const char *text, int64_t *out)
{
  const unsigned char *p = (const unsigned char *) text;
  uint64_t magnitude = 0;
  uint64_t limit;
  int negative = 0;

  if (!text || !out || !*p)
    return 0;
  if (*p == '-' || *p == '+')
  {
    negative = (*p == '-');
    ++p;
  }
  if (!*p)
    return 0;
  limit = negative ? (uint64_t) INT64_MAX + 1u : (uint64_t) INT64_MAX;
  while (*p)
  {
    unsigned digit;
    if (*p < '0' || *p > '9')
      return 0;
    digit = (unsigned) (*p - '0');
    if (magnitude > (limit - digit) / 10u)
      return 0;
    magnitude = magnitude * 10u + digit;
    ++p;
  }
  if (negative)
    *out = magnitude == (uint64_t) INT64_MAX + 1u
             ? INT64_MIN
             : -(int64_t) magnitude;
  else
    *out = (int64_t) magnitude;
  return 1;
}

ScVmResult
sc_vm_value_to_string (ScVm *vm, ScValue value, char **out_text)
{
  BclError e;
  char *text;
  if (!vm || !out_text)
    return SC_ERR_INTERNAL;
  *out_text = NULL;
  if (!sc_vm_value_valid (vm, value))
    return SC_ERR_INVALID_VALUE;
  e = bcl_pushContext (vm->ctx);
  if (e != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_CONTEXT_PUSH, "bcl context push failed");
  text = bcl_string_keep (sc_vm_value_number (vm, value));
  bcl_popContext ();
  if (!text)
    return vm_fail (vm, SC_ERR_ARITHMETIC, "BCL string conversion failed");
  *out_text = text;
  return SC_VM_OK;
}

ScVmResult
sc_vm_export_value (ScVm *vm, ScValue value, ScExternalValue *result)
{
  ScVmResult rc;
  char *text = NULL;
  int64_t integer;
  if (!result)
    return SC_ERR_INTERNAL;
  memset (result, 0, sizeof (*result));
  rc = sc_vm_value_to_string (vm, value, &text);
  if (rc != SC_VM_OK)
    return rc;
  if (bcl_num_scale (sc_vm_value_number (vm, value)) == 0u
      && parse_int64_decimal (text, &integer))
  {
    free (text);
    result->type = SC_EXTERNAL_INTEGER;
    result->value.integer = integer;
  }
  else
  {
    result->type = SC_EXTERNAL_TEXT;
    result->value.text = text;
  }
  return SC_VM_OK;
}

ScVmResult
sc_vm_import_int64 (ScVm *vm, int64_t integer, ScValue *out_value)
{
  BclBigDig magnitude;
  BclNumber number;
  BclError error;

  if (!vm || !out_value)
    return SC_ERR_INTERNAL;
  *out_value = SC_INVALID_VALUE;
  magnitude = integer < 0
                ? (BclBigDig) ((uint64_t) (-(integer + 1)) + 1u)
                : (BclBigDig) integer;
  error = bcl_pushContext (vm->ctx);
  if (error != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_CONTEXT_PUSH, "bcl context push failed");
  number = bcl_bigdig2num (magnitude);
  if (bcl_err (number) == BCL_ERROR_NONE && integer < 0)
    bcl_num_setNeg (number, true);
  bcl_popContext ();
  if (bcl_err (number) != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_ARITHMETIC, "cannot import integer");
  return sc_vm_new_value (vm, number, out_value);
}

static bool
sc_external_decimal_valid (const char *text, size_t text_len, char prefix)
{
  size_t i = 1u;
  bool radix = false;
  bool digit = false;

  if (!text || prefix == '\0' || text_len <= i || text[0] != prefix)
    return false;
  if (text[i] == '-')
    ++i;
  if (i == text_len)
    return false;
  for (; i < text_len; ++i)
  {
    unsigned char ch = (unsigned char) text[i];
    if (ch == '.')
    {
      if (radix)
        return false;
      radix = true;
    }
    else if (ch >= (unsigned char) '0' && ch <= (unsigned char) '9')
      digit = true;
    else
      return false;
  }
  return digit;
}

ScVmResult
sc_vm_import_numeric_text (ScVm *vm, const char *text, size_t text_len,
                           ScValue *out_value)
{
  char *decimal;
  BclNumber number;
  BclError error;

  if (!vm || !out_value)
    return SC_ERR_INTERNAL;
  *out_value = SC_INVALID_VALUE;
  if (!sc_external_decimal_valid (text, text_len, vm->numeric_text_prefix))
    return vm_fail (vm, SC_ERR_TYPE, "invalid external numeric text");
  size_t decimal_len = text_len - 1u;
  decimal = malloc (decimal_len + 1u);
  if (!decimal)
    return vm_fail (vm, SC_ERR_NO_MEM, "numeric text allocation failed");
  memcpy (decimal, text + 1u, decimal_len);
  decimal[decimal_len] = '\0';
  error = bcl_pushContext (vm->ctx);
  if (error != BCL_ERROR_NONE)
  {
    free (decimal);
    return vm_fail (vm, SC_ERR_CONTEXT_PUSH, "bcl context push failed");
  }
  number = bcl_parse (decimal);
  bcl_popContext ();
  free (decimal);
  if (bcl_err (number) != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_ARITHMETIC, "cannot import numeric text");
  return sc_vm_new_value (vm, number, out_value);
}

static ScVmResult
alloc_slot (ScVm *vm, BclNumber n, int32_t rc, ScValue *out)
{
  uint32_t id;
  if (bcl_err (n) != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_ARITHMETIC, "BCL number error");
  if (vm->free_head != SC_NO_FREE_SLOT)
  {
    id = vm->free_head;
    vm->free_head = vm->slots[id].next_free;
  }
  else
  {
    if (!grow_array
	((void **) &vm->slots, sizeof (*vm->slots), &vm->slot_capacity,
	 vm->slot_count + 1u, UINT32_MAX - 1u))
      return vm_fail (vm, SC_ERR_NO_MEM, "out of slot memory");
    id = vm->slot_count++;
  }
  vm->slots[id].num = n;
  vm->slots[id].refcnt = rc;
  vm->slots[id].next_free = SC_NO_FREE_SLOT;
  *out = id;
  return SC_VM_OK;
}

ScVmResult
sc_vm_new_value (ScVm *vm, BclNumber n, ScValue *out)
{
  return alloc_slot (vm, n, 1, out);
}

ScVmResult
sc_vm_new_permanent_value (ScVm *vm, BclNumber n, ScValue *out)
{
  return alloc_slot (vm, n, -1, out);
}

ScVmResult
sc_vm_retain_value (ScVm *vm, ScValue v)
{
  if (!sc_vm_value_valid (vm, v))
    return vm_fail (vm, SC_ERR_INVALID_VALUE, "retain invalid value");
  if (vm->slots[v].refcnt > 0)
  {
    if (vm->slots[v].refcnt == INT32_MAX)
      return vm_fail (vm, SC_ERR_INTERNAL, "refcount overflow");
    vm->slots[v].refcnt++;
  }
  return SC_VM_OK;
}

ScVmResult
sc_vm_release_value (ScVm *vm, ScValue v)
{
  ScSlot *s;
  if (!sc_vm_value_valid (vm, v))
    return vm_fail (vm, SC_ERR_INVALID_VALUE, "release invalid value");
  s = &vm->slots[v];
  if (s->refcnt < 0)
    return SC_VM_OK;
  if (--s->refcnt == 0)
  {
    bcl_num_free (s->num);
    s->next_free = vm->free_head;
    vm->free_head = v;
  }
  return SC_VM_OK;
}

static ScVmResult
make_perm_int (ScVm *vm, BclBigDig x, ScValue *out)
{
  BclNumber n = bcl_bigdig2num (x);
  if (bcl_err (n) != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_ARITHMETIC, "cannot create integer");
  return sc_vm_new_permanent_value (vm, n, out);
}

void
sc_vm_clear_error (ScVm *vm)
{
  if (vm)
    memset (&vm->error, 0, sizeof (vm->error));
}

const ScVmError *
sc_vm_error (const ScVm *vm)
{
  return vm ? &vm->error : NULL;
}

ScVmResult
sc_vm_init (ScVm *vm, const ScSecurityLimits *limits)
{
  ScSecurityLimits d;
  BclError e;
  ScVmError saved_error;
  ScVmResult rc;
  if (!vm)
    return SC_ERR_INTERNAL;
  memset (vm, 0, sizeof (*vm));
  vm->health = SC_VM_HEALTH_READY;
  vm->call_result = SC_INVALID_VALUE;
  vm->free_head = SC_NO_FREE_SLOT;
  vm->numeric_text_prefix = '#';
  if (limits)
    vm->limits = *limits;
  else
  {
    sc_security_limits_default (&d);
    vm->limits = d;
  }
  e = bcl_start ();
  if (e != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_CONTEXT_CREATE, "bcl_start failed");
  vm->bcl_started = true;
  e = bcl_init ();
  if (e != BCL_ERROR_NONE)
  {
    rc = vm_fail (vm, SC_ERR_CONTEXT_CREATE, "bcl_init failed");
    goto fail;
  }
  vm->bcl_initialized = true;
  vm->ctx = bcl_ctxt_create ();
  if (!vm->ctx)
  {
    rc = vm_fail (vm, SC_ERR_CONTEXT_CREATE, "bcl context create failed");
    goto fail;
  }
  vm->context_created = true;
  e = bcl_pushContext (vm->ctx);
  if (e != BCL_ERROR_NONE)
  {
    rc = vm_fail (vm, SC_ERR_CONTEXT_PUSH, "bcl context push failed");
    goto fail;
  }
  vm->context_pushed = true;
  if (sc_symbol_table_init (&vm->symbols, 257u) != SC_VM_OK)
  {
    rc = vm_fail (vm, SC_ERR_NO_MEM, "symbol table init failed");
    goto fail;
  }
  vm->symbols_initialized = true;
  if (make_perm_int (vm, 0, &vm->zero_value) != SC_VM_OK
      || make_perm_int (vm, 1, &vm->one_value) != SC_VM_OK)
  {
    rc = vm->error.code;
    goto fail;
  }
  bcl_gc ();
  return SC_VM_OK;

fail:
  saved_error = vm->error;
  sc_vm_destroy (vm);
  vm->error = saved_error;
  return rc;
}

ScVmResult
sc_vm_set_numeric_text_prefix (ScVm *vm, char prefix)
{
  if (!vm)
    return SC_ERR_INVALID_PREFIX;
  if (!sc_numeric_prefix_valid (prefix))
    return vm_fail (vm, SC_ERR_INVALID_PREFIX,
                    "invalid numeric text prefix");
  vm->numeric_text_prefix = prefix;
  return SC_VM_OK;
}

char
sc_vm_get_numeric_text_prefix (const ScVm *vm)
{
  return vm ? vm->numeric_text_prefix : '\0';
}

void
sc_proc_destroy (ScProc *p)
{
  if (p)
  {
    free (p->code);
    memset (p, 0, sizeof (*p));
  }
}

void
sc_vm_destroy (ScVm *vm)
{
  uint32_t i;
  if (!vm)
    return;
  sc_vm_clear_frames (vm);
#if SC_ENABLE_PERSISTENT_STATE
  sc_vm_state_rollback (vm);
#endif
  for (i = 0; i < vm->proc_count; ++i)
    sc_proc_destroy (&vm->procs[i]);
  if (vm->context_created && vm->ctx)
    bcl_ctxt_freeNums (vm->ctx);
  free (vm->frames);
  free (vm->slots);
  free (vm->procs);
  free (vm->cfuncs);
  if (vm->symbols_initialized)
    sc_symbol_table_destroy (&vm->symbols);
  if (vm->context_pushed)
  {
    bcl_gc ();
    bcl_popContext ();
    vm->context_pushed = false;
  }
  if (vm->context_created && vm->ctx)
  {
    bcl_ctxt_free (vm->ctx);
    vm->ctx = NULL;
    vm->context_created = false;
    if (vm->bcl_initialized)
      bcl_gc ();
  }
  if (vm->bcl_initialized)
  {
    bcl_free ();
    vm->bcl_initialized = false;
  }
  if (vm->bcl_started)
  {
    bcl_end ();
    vm->bcl_started = false;
  }
  memset (vm, 0, sizeof (*vm));
}

#if SC_ENABLE_PERSISTENT_STATE
ScVmResult
sc_persistent_state_init (ScVm *vm, ScPersistentState *state, uint16_t count)
{
  uint16_t i;
  if (!vm || !state)
    return SC_ERR_INTERNAL;
  if (state->initialized || count == 0u || count > SC_PERSISTENT_STATE_SLOTS)
    return vm_fail (vm, SC_ERR_STATE_INVALID_COUNT, "invalid persistent state count");
  memset (state, 0, sizeof (*state));
  state->count = count;
  for (i = 0; i < count; ++i)
    state->values[i] = vm->zero_value;
  state->initialized = true;
  return SC_VM_OK;
}

void
sc_persistent_state_destroy (ScVm *vm, ScPersistentState *state)
{
  uint16_t i;
  if (!vm || !state || !state->initialized)
    return;
  if (vm->state_binding.active && vm->state_binding.state == state)
    sc_vm_state_rollback (vm);
  for (i = 0; i < state->count; ++i)
    if (sc_vm_value_valid (vm, state->values[i]))
      sc_vm_release_value (vm, state->values[i]);
  memset (state, 0, sizeof (*state));
}

ScVmResult
sc_vm_state_begin (ScVm *vm, ScPersistentState *state, ScStateAccess access)
{
  uint16_t i;
  if (!vm || !state || !state->initialized)
    return vm_fail (vm, SC_ERR_STATE_INVALID_COUNT, "persistent state is not initialized");
  if (vm->state_binding.active)
    return vm_fail (vm, SC_ERR_STATE_ALREADY_ACTIVE, "persistent state callback already active");
  if (access != SC_STATE_ACCESS_READ_ONLY && access != SC_STATE_ACCESS_READ_WRITE)
    return vm_fail (vm, SC_ERR_INTERNAL, "invalid persistent state access mode");
  memset (&vm->state_binding, 0, sizeof (vm->state_binding));
  vm->state_binding.state = state;
  vm->state_binding.count = state->count;
  vm->state_binding.access = access;
  for (i = 0; i < state->count; ++i)
  {
    ScVmResult rc = sc_vm_retain_value (vm, state->values[i]);
    if (rc != SC_VM_OK)
    {
      while (i > 0u)
        sc_vm_release_value (vm, vm->state_binding.working[--i]);
      memset (&vm->state_binding, 0, sizeof (vm->state_binding));
      return rc;
    }
    vm->state_binding.working[i] = state->values[i];
  }
  vm->state_binding.active = true;
  return SC_VM_OK;
}

void
sc_vm_state_rollback (ScVm *vm)
{
  uint16_t i;
  if (!vm || !vm->state_binding.active)
    return;
  for (i = 0; i < vm->state_binding.count; ++i)
    if (sc_vm_value_valid (vm, vm->state_binding.working[i]))
      sc_vm_release_value (vm, vm->state_binding.working[i]);
  memset (&vm->state_binding, 0, sizeof (vm->state_binding));
}

ScVmResult
sc_vm_state_commit (ScVm *vm)
{
  ScPersistentState *state;
  uint16_t i;
  if (!vm || !vm->state_binding.active)
    return vm_fail (vm, SC_ERR_STATE_NOT_ACTIVE, "persistent state callback is not active");
  if (vm->state_binding.access != SC_STATE_ACCESS_READ_WRITE)
    return vm_fail (vm, SC_ERR_STATE_READ_ONLY, "persistent state is read-only");
  state = vm->state_binding.state;
  for (i = 0; i < state->count; ++i)
  {
    if (sc_vm_value_valid (vm, state->values[i]))
      sc_vm_release_value (vm, state->values[i]);
    state->values[i] = vm->state_binding.working[i];
    vm->state_binding.working[i] = SC_INVALID_VALUE;
  }
  memset (&vm->state_binding, 0, sizeof (vm->state_binding));
  return SC_VM_OK;
}

bool sc_vm_state_active (const ScVm *vm) { return vm && vm->state_binding.active; }
uint16_t sc_vm_state_count (const ScVm *vm) { return sc_vm_state_active (vm) ? vm->state_binding.count : 0u; }

ScVmResult
sc_vm_state_get (ScVm *vm, uint16_t index, ScValue *out_value)
{
  if (!vm || !out_value)
    return SC_ERR_INTERNAL;
  if (!vm->state_binding.active)
    return vm_fail (vm, SC_ERR_STATE_NOT_ACTIVE, "state_get is only available during a state callback");
  if (index >= vm->state_binding.count)
    return vm_fail (vm, SC_ERR_STATE_RANGE, "persistent state index out of range");
  *out_value = vm->state_binding.working[index];
  return SC_VM_OK;
}

ScVmResult
sc_vm_state_set (ScVm *vm, uint16_t index, ScValue value)
{
  ScValue old;
  ScVmResult rc;
  if (!vm)
    return SC_ERR_INTERNAL;
  if (!vm->state_binding.active)
    return vm_fail (vm, SC_ERR_STATE_NOT_ACTIVE, "state_set is only available during a state callback");
  if (vm->state_binding.access != SC_STATE_ACCESS_READ_WRITE)
    return vm_fail (vm, SC_ERR_STATE_READ_ONLY, "persistent state is read-only");
  if (index >= vm->state_binding.count)
    return vm_fail (vm, SC_ERR_STATE_RANGE, "persistent state index out of range");
  rc = sc_vm_retain_value (vm, value);
  if (rc != SC_VM_OK)
    return rc;
  old = vm->state_binding.working[index];
  vm->state_binding.working[index] = value;
  return sc_vm_release_value (vm, old);
}
#endif

ScVmResult
sc_vm_register_cfunc (ScVm *vm, const char *name, uint16_t argc, ScCFunc fn,
		      ScSymbolId *out)
{
  ScSymbolId sid;
  uint32_t cap = vm->cfunc_capacity;
  if (!vm || !name || !fn || !out)
    return SC_ERR_INTERNAL;
  if (!grow_array
      ((void **) &vm->cfuncs, sizeof (*vm->cfuncs), &cap,
       vm->cfunc_count + 1u, SC_MAX_SYMBOLS))
    return vm_fail (vm, SC_ERR_NO_MEM, "cfunc table allocation failed");
  vm->cfunc_capacity = cap;
  if (sc_symbol_add
      (&vm->symbols, sc_slice_make (name, strlen (name)), argc, SC_SYM_CFUNC,
       &sid) != SC_VM_OK)
    return vm_fail (vm, SC_ERR_INVALID_SYMBOL, "duplicate cfunc");
  vm->cfuncs[vm->cfunc_count] = fn;
  vm->symbols.symbols[sid].target.cfunc_id = vm->cfunc_count++;
  *out = sid;
  return SC_VM_OK;
}

ScVmResult
sc_vm_declare_proc (ScVm *vm, const char *name, uint16_t argc,
		    ScSymbolId *out)
{
  ScSymbolId sid;
  if (sc_symbol_lookup (&vm->symbols, sc_slice_make (name, strlen (name)), &sid) == SC_VM_OK)
  {
    ScSymbol *s = &vm->symbols.symbols[sid];
    if (s->kind == SC_SYM_EMPTY && s->argc == argc)
    {
      *out = sid;
      return SC_VM_OK;
    }
    return vm_fail (vm, SC_ERR_INVALID_SYMBOL, "duplicate procedure");
  }
  if (sc_symbol_add
      (&vm->symbols, sc_slice_make (name, strlen (name)), argc, SC_SYM_EMPTY,
       out) != SC_VM_OK)
    return vm_fail (vm, SC_ERR_NO_MEM, "cannot declare proc");
  return SC_VM_OK;
}

ScVmResult
sc_vm_install_proc (ScVm *vm, ScSymbolId sid, ScProc *proc, ScProcId *out)
{
  uint32_t cap = vm->proc_capacity;
  ScProcId id;
  if (!vm || !proc || sid >= vm->symbols.count)
    return SC_ERR_INVALID_SYMBOL;
  if (vm->symbols.symbols[sid].kind != SC_SYM_EMPTY)
    return vm_fail (vm, SC_ERR_INVALID_SYMBOL, "procedure already installed");
  if (!grow_array
      ((void **) &vm->procs, sizeof (*vm->procs), &cap, vm->proc_count + 1u,
       vm->limits.max_procs))
    return vm_fail (vm, SC_ERR_NO_MEM, "proc table allocation failed");
  vm->proc_capacity = cap;
  id = vm->proc_count++;
  vm->procs[id] = *proc;
  memset (proc, 0, sizeof (*proc));
  vm->symbols.symbols[sid].kind = SC_SYM_PROC;
  vm->symbols.symbols[sid].target.proc_id = id;
  if (out)
    *out = id;
  return SC_VM_OK;
}

ScFrame *
sc_vm_current_frame (ScVm *vm)
{
  return vm && vm->frame_count ? &vm->frames[vm->frame_count - 1u] : NULL;
}

const ScFrame *
sc_vm_current_frame_const (const ScVm *vm)
{
  return vm && vm->frame_count ? &vm->frames[vm->frame_count - 1u] : NULL;
}

ScVmResult
sc_vm_push_frame (ScVm *vm, ScProc *p, const ScValue *args, uint16_t argc)
{
  ScFrame *fr;
  uint32_t cap = vm->frame_capacity;
  uint16_t i;
  if (argc != p->argc)
    return vm_fail (vm, SC_ERR_ARG_COUNT, "wrong argument count");
  if (vm->frame_count >= vm->limits.max_frames)
    return vm_fail (vm, SC_ERR_FRAME_OVERFLOW, "frame limit");
  if (!grow_array
      ((void **) &vm->frames, sizeof (*vm->frames), &cap,
       vm->frame_count + 1u, vm->limits.max_frames))
    return vm_fail (vm, SC_ERR_NO_MEM, "frame allocation failed");
  vm->frame_capacity = (uint16_t) cap;
  fr = &vm->frames[vm->frame_count];
  memset (fr, 0, sizeof (*fr));
  fr->proc = p;
  fr->slot_count = p->slot_count;
  fr->capacity = p->slot_count + p->max_stack;
  fr->values = malloc (sizeof (ScValue) * (fr->capacity ? fr->capacity : 1u));
  if (!fr->values)
    return vm_fail (vm, SC_ERR_NO_MEM, "frame values allocation failed");
  for (i = 0; i < argc; ++i)
  {
    fr->values[i] = args[i];
    sc_vm_retain_value (vm, args[i]);
  }
  for (; i < p->slot_count; ++i)
    fr->values[i] = vm->zero_value;
  fr->sp = p->slot_count;
  vm->frame_count++;
  return SC_VM_OK;
}

ScVmResult
sc_vm_push_value (ScVm *vm, ScValue v)
{
  ScFrame *fr = sc_vm_current_frame (vm);
  if (!fr || !sc_vm_value_valid (vm, v))
    return vm_fail (vm, SC_ERR_INVALID_VALUE, "push invalid value");
  if (fr->sp >= fr->capacity)
    return vm_fail (vm, SC_ERR_STACK_OVERFLOW, "stack overflow");
  if (sc_vm_retain_value (vm, v) != SC_VM_OK)
    return vm->error.code;
  fr->values[fr->sp++] = v;
  return SC_VM_OK;
}

ScVmResult
sc_vm_pop_value (ScVm *vm, ScValue *out)
{
  ScFrame *fr = sc_vm_current_frame (vm);
  if (!fr || fr->sp <= fr->slot_count)
    return vm_fail (vm, SC_ERR_STACK_UNDERFLOW, "stack underflow");
  *out = fr->values[--fr->sp];
  return SC_VM_OK;
}

ScVmResult
sc_vm_peek_value (const ScVm *vm, ScValue *out)
{
  const ScFrame *fr = sc_vm_current_frame_const (vm);
  if (!fr || fr->sp <= fr->slot_count)
    return SC_ERR_STACK_UNDERFLOW;
  *out = fr->values[fr->sp - 1u];
  return SC_VM_OK;
}

ScVmResult
sc_vm_load_local (ScVm *vm, uint16_t slot, ScValue *out)
{
  ScFrame *fr = sc_vm_current_frame (vm);
  if (!fr || slot >= fr->slot_count)
    return vm_fail (vm, SC_ERR_INVALID_SLOT, "bad local slot");
  *out = fr->values[slot];
  return SC_VM_OK;
}

ScVmResult
sc_vm_store_local (ScVm *vm, uint16_t slot, ScValue v)
{
  ScFrame *fr = sc_vm_current_frame (vm);
  ScValue old;
  if (!fr || slot >= fr->slot_count || !sc_vm_value_valid (vm, v))
    return vm_fail (vm, SC_ERR_INVALID_SLOT, "bad local store");
  old = fr->values[slot];
  fr->values[slot] = v;		/* takes ownership of popped value */
  if (old != vm->zero_value)
    return sc_vm_release_value (vm, old);
  return SC_VM_OK;
}

ScVmResult
sc_vm_pop_frame (ScVm *vm, ScValue result)
{
  ScFrame *fr;
  uint16_t i;
  if (!vm->frame_count)
    return vm_fail (vm, SC_ERR_INTERNAL, "no frame");
  fr = &vm->frames[vm->frame_count - 1u];
  for (i = 0; i < fr->sp; ++i)
    if (fr->values[i] != result || i < fr->slot_count)
      sc_vm_release_value (vm, fr->values[i]);
  free (fr->values);
  memset (fr, 0, sizeof (*fr));
  vm->frame_count--;
  if (vm->frame_count)
  {
    ScVmResult rc = sc_vm_push_value (vm, result);
    sc_vm_release_value (vm, result);
    return rc;
  }
  vm->call_result = result;
  vm->call_result_set = true;
  return SC_VM_OK;
}

uint16_t
sc_vm_argc (const ScVm *vm)
{
  return vm ? vm->call_argc : 0;
}

ScVmResult
sc_vm_arg (const ScVm *vm, uint16_t i, ScValue *out)
{
  if (!vm || !out || i >= vm->call_argc)
    return SC_ERR_ARG_COUNT;
  *out = vm->call_args[i];
  return SC_VM_OK;
}

ScVmResult
sc_vm_set_result (ScVm *vm, ScValue v)
{
  if (!sc_vm_value_valid (vm, v))
    return vm_fail (vm, SC_ERR_INVALID_VALUE, "invalid builtin result");
  vm->call_result = v;
  vm->call_result_set = true;
  return SC_VM_OK;
}

ScVmResult
sc_vm_value_is_zero (ScVm *vm, ScValue v, bool *out)
{
  if (!sc_vm_value_valid (vm, v))
    return vm_fail (vm, SC_ERR_INVALID_VALUE, "invalid truth value");
  *out = bcl_cmp (vm->slots[v].num, vm->slots[vm->zero_value].num) == 0;
  return SC_VM_OK;
}

ScVmResult
sc_vm_dispatch_call (ScVm *vm, ScSymbolId sid, uint16_t argc)
{
  ScFrame *fr = sc_vm_current_frame (vm);
  ScSymbol *s;
  uint16_t i;
  ScValue *args;
  ScVmResult rc;
  if (!fr || sid >= vm->symbols.count)
    return vm_fail (vm, SC_ERR_INVALID_SYMBOL, "unknown symbol");
  s = &vm->symbols.symbols[sid];
  if (argc != s->argc)
    return vm_fail (vm, SC_ERR_ARG_COUNT, "wrong call arity");
  if (fr->sp < fr->slot_count + argc)
    return vm_fail (vm, SC_ERR_STACK_UNDERFLOW, "call stack underflow");
  args = argc ? malloc (sizeof (*args) * argc) : NULL;
  if (argc && !args)
    return vm_fail (vm, SC_ERR_NO_MEM, "call args allocation failed");
  for (i = argc; i > 0; --i)
    sc_vm_pop_value (vm, &args[i - 1u]);
  if (s->kind == SC_SYM_CFUNC)
  {
    vm->call_args = args;
    vm->call_argc = argc;
    vm->call_result_set = false;
    rc = vm->cfuncs[s->target.cfunc_id] (vm);
    vm->call_args = NULL;
    vm->call_argc = 0;
    /* BCL arithmetic uses a per-thread scratch pool.  Results have already
       been promoted into context-owned BclNumbers by the builtin, so scratch
       storage can and should be reclaimed at every builtin boundary. */
    bcl_gc ();
    for (i = 0; i < argc; ++i)
      sc_vm_release_value (vm, args[i]);
    free (args);
    if (rc != SC_VM_OK)
      return rc;
    if (!vm->call_result_set)
      return vm_fail (vm, SC_ERR_NO_RETURN_VALUE,
		      "builtin did not set result");
    rc = sc_vm_push_value (vm, vm->call_result);
    sc_vm_release_value (vm, vm->call_result);
    vm->call_result_set = false;
    return rc;
  }
  if (s->kind == SC_SYM_EMPTY)
  {
    free (args);
    return vm_fail (vm, SC_ERR_EMPTY_PROC, "empty procedure");
  }
  rc = sc_vm_push_frame (vm, &vm->procs[s->target.proc_id], args, argc);
  for (i = 0; i < argc; ++i)
    sc_vm_release_value (vm, args[i]);
  free (args);
  return rc;
}

ScVmResult
sc_vm_step (ScVm *vm)
{
  ScFrame *fr = sc_vm_current_frame (vm);
  ScInstr in;
  ScValue v = SC_INVALID_VALUE;
  bool z = false;
  ScVmResult rc;
  if (!fr)
    return vm_fail (vm, SC_ERR_RUNTIME, "no frame to execute");
  if (++vm->instruction_count > vm->limits.max_instructions)
    return vm_fail (vm, SC_ERR_MAX_INSTRUCTIONS, "instruction limit");
  if (fr->ip >= fr->proc->code_count)
    return vm_fail (vm, SC_ERR_RUNTIME, "fell off procedure");
  in = fr->proc->code[fr->ip++];
  switch ((ScVmOpCode) in.op)
  {
  case SC_OP_CONST:
    return sc_vm_push_value (vm, in.b);
  case SC_OP_LOAD:
    rc = sc_vm_load_local (vm, in.a, &v);
    return rc == SC_VM_OK ? sc_vm_push_value (vm, v) : rc;
  case SC_OP_STORE:
    rc = sc_vm_pop_value (vm, &v);
    return rc == SC_VM_OK ? sc_vm_store_local (vm, in.a, v) : rc;
  case SC_OP_POP:
    rc = sc_vm_pop_value (vm, &v);
    return rc == SC_VM_OK ? sc_vm_release_value (vm, v) : rc;
  case SC_OP_CALL:
    return sc_vm_dispatch_call (vm, in.b, in.a);
  case SC_OP_JMP:
    fr->ip = in.b;
    return SC_VM_OK;
  case SC_OP_JZ:
  case SC_OP_JNZ:
    rc = sc_vm_pop_value (vm, &v);
    if (rc != SC_VM_OK)
      return rc;
    rc = sc_vm_value_is_zero (vm, v, &z);
    sc_vm_release_value (vm, v);
    if (rc != SC_VM_OK)
      return rc;
    if ((in.op == SC_OP_JZ && z) || (in.op == SC_OP_JNZ && !z))
      fr->ip = in.b;
    return SC_VM_OK;
  case SC_OP_RET:
    rc = sc_vm_pop_value (vm, &v);
    if (rc != SC_VM_OK)
      return vm_fail (vm, SC_ERR_NO_RETURN_VALUE, "return without value");
    return sc_vm_pop_frame (vm, v);
  default:
    return vm_fail (vm, SC_ERR_INVALID_OPCODE, "invalid opcode");
  }
}

ScVmResult
sc_vm_run (ScVm *vm, ScValue *out)
{
  ScVmResult rc;
  if (!vm || !out)
    return SC_ERR_INTERNAL;
  if (vm->health == SC_VM_HEALTH_FATAL_ERROR)
    return vm->error.code != SC_VM_OK ? vm->error.code : SC_ERR_INTERNAL;
  vm->call_result_set = false;
  while (vm->frame_count)
  {
    rc = sc_vm_step (vm);
    if (rc != SC_VM_OK)
      return sc_vm_finish_error (vm, rc);
  }
  if (!vm->call_result_set)
  {
    rc = vm_fail (vm, SC_ERR_NO_RETURN_VALUE, "no top-level result");
    return sc_vm_finish_error (vm, rc);
  }
  *out = vm->call_result;
  vm->call_result_set = false;
  return SC_VM_OK;
}

ScVmResult
sc_vm_call (ScVm *vm, ScSymbolId sid, const ScValue *args, uint16_t argc,
	    ScValue *out)
{
  ScSymbol *s;
  ScVmResult rc;
  if (!vm || !out)
    return SC_ERR_INTERNAL;
  if (vm->health == SC_VM_HEALTH_FATAL_ERROR)
    return vm->error.code != SC_VM_OK ? vm->error.code : SC_ERR_INTERNAL;
  if (sid >= vm->symbols.count)
    return SC_ERR_INVALID_SYMBOL;
  s = &vm->symbols.symbols[sid];
  if (argc != s->argc)
    return SC_ERR_ARG_COUNT;
  vm->instruction_count = 0;
  if (s->kind == SC_SYM_PROC)
  {
    rc = sc_vm_push_frame (vm, &vm->procs[s->target.proc_id], args, argc);
    if (rc != SC_VM_OK)
      return rc;
    return sc_vm_run (vm, out);
  }
  if (s->kind == SC_SYM_CFUNC)
  {
    vm->call_args = args;
    vm->call_argc = argc;
    vm->call_result_set = false;
    rc = vm->cfuncs[s->target.cfunc_id] (vm);
    vm->call_args = NULL;
    vm->call_argc = 0;
    /* See sc_vm_dispatch_call(): do not let BCL scratch allocations grow
       across calls made directly through the embedding API. */
    bcl_gc ();
    if (rc != SC_VM_OK)
      return sc_vm_finish_error (vm, rc);
    if (!vm->call_result_set)
    {
      rc = vm_fail (vm, SC_ERR_NO_RETURN_VALUE, "builtin did not set result");
      return sc_vm_finish_error (vm, rc);
    }
    *out = vm->call_result;
    vm->call_result_set = false;
    return SC_VM_OK;
  }
  return SC_ERR_EMPTY_PROC;
}

ScVmResult
sc_vm_call_slice (ScVm *vm, ScSlice name, const ScValue *args,
                  uint16_t argc, ScValue *out)
{
  ScSymbolId sid;
  ScVmResult rc;
  if (!vm || !name.p || name.len == 0u)
    return SC_ERR_INVALID_SYMBOL;
  if (isdigit ((unsigned char) name.p[0]))
    return vm_fail (vm, SC_ERR_INVALID_SYMBOL, "private procedure");
  rc = sc_symbol_lookup (&vm->symbols, name, &sid);
  return rc == SC_VM_OK ? sc_vm_call (vm, sid, args, argc, out) : rc;
}

ScVmResult
sc_vm_call_name (ScVm *vm, const char *name, size_t len, const ScValue *args,
                 uint16_t argc, ScValue *out)
{
  return sc_vm_call_slice (vm, sc_slice_make (name, len), args, argc, out);
}

/* Builtins used by tests and available to embedders through registration. */
static ScVmResult
builtin_finish_number (ScVm *vm, BclNumber n)
{
  ScValue v = SC_INVALID_VALUE;
  if (bcl_err (n) != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_ARITHMETIC, "BCL arithmetic error");
  if (sc_vm_new_value (vm, n, &v) != SC_VM_OK)
  {
    bcl_num_free (n);
    return vm->error.code;
  }
  return sc_vm_set_result (vm, v);
}

static ScVmResult
builtin_get_scale (ScVm *vm)
{
  return builtin_finish_number
    (vm, bcl_bigdig2num ((BclBigDig) bcl_ctxt_scale (vm->ctx)));
}

static ScVmResult
builtin_places_arg (ScVm *vm, uint16_t index, size_t *places)
{
  ScValue v;
  BclBigDig x;
  if (sc_vm_arg (vm, index, &v) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  if (bcl_num_neg (sc_vm_value_number (vm, v))
      || bcl_num_scale (sc_vm_value_number (vm, v)) != 0)
    return vm_fail (vm, SC_ERR_TYPE,
                    "places must be a non-negative integer");
  if (bcl_bigdig_keep (sc_vm_value_number (vm, v), &x) != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_TYPE,
                    "places must be a non-negative integer");
  if ((BclBigDig) (size_t) x != x)
    return vm_fail (vm, SC_ERR_TYPE, "places is too large");
  *places = (size_t) x;
  return SC_VM_OK;
}

static ScVmResult
builtin_random_bound (ScVm *vm, ScValue bound)
{
  BclNumber n = sc_vm_value_number (vm, bound);
  if (bcl_num_neg (n) || bcl_num_scale (n) != 0)
    return vm_fail (vm, SC_ERR_TYPE,
                    "random bound must be a non-negative integer");
  return SC_VM_OK;
}

static ScVmResult
builtin_irand (ScVm *vm)
{
  ScValue bound;
  ScVmResult rc;
  if (sc_vm_arg (vm, 0, &bound) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  rc = builtin_random_bound (vm, bound);
  if (rc != SC_VM_OK)
    return rc;
  return builtin_finish_number
    (vm, bcl_irand_keep (sc_vm_value_number (vm, bound)));
}

static ScVmResult
builtin_frand (ScVm *vm)
{
  size_t places = 0u;
  ScVmResult rc = builtin_places_arg (vm, 0, &places);
  if (rc != SC_VM_OK)
    return rc;
  return builtin_finish_number (vm, bcl_frand (places));
}

static ScVmResult
builtin_ifrand (ScVm *vm)
{
  ScValue bound = SC_INVALID_VALUE;
  size_t places = 0u;
  ScVmResult rc;
  if (sc_vm_arg (vm, 0, &bound) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  rc = builtin_random_bound (vm, bound);
  if (rc != SC_VM_OK)
    return rc;
  rc = builtin_places_arg (vm, 1, &places);
  if (rc != SC_VM_OK)
    return rc;
  return builtin_finish_number
    (vm, bcl_ifrand_keep (sc_vm_value_number (vm, bound), places));
}

static ScVmResult
builtin_rand_seed (ScVm *vm)
{
  ScValue seed;
  if (sc_vm_arg (vm, 0, &seed) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  if (bcl_rand_seedWithNum_keep (sc_vm_value_number (vm, seed))
      != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_ARITHMETIC, "invalid random seed");
  return builtin_finish_number (vm, bcl_rand_seed2num ());
}

static ScVmResult
builtin_rand_seed_get (ScVm *vm)
{
  return builtin_finish_number (vm, bcl_rand_seed2num ());
}

static ScVmResult
builtin_rand_reseed (ScVm *vm)
{
  bcl_rand_reseed ();
  return builtin_finish_number (vm, bcl_rand_seed2num ());
}

static ScVmResult
builtin_abs (ScVm *vm)
{
  ScValue a;
  BclNumber n;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  n = bcl_dup (sc_vm_value_number (vm, a));
  if (bcl_err (n) != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_ARITHMETIC, "BCL duplicate error");
  bcl_num_setNeg (n, false);
  return builtin_finish_number (vm, n);
}

static ScVmResult
builtin_num_scale (ScVm *vm)
{
  ScValue a;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  return builtin_finish_number
    (vm, bcl_bigdig2num ((BclBigDig) bcl_num_scale
                         (sc_vm_value_number (vm, a))));
}

static ScVmResult
builtin_length (ScVm *vm)
{
  ScValue a;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  return builtin_finish_number
    (vm, bcl_bigdig2num ((BclBigDig) bcl_num_len
                         (sc_vm_value_number (vm, a))));
}

static ScVmResult
builtin_set_scale (ScVm *vm)
{
  ScValue a;
  BclBigDig x;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  if (bcl_bigdig_keep (sc_vm_value_number (vm, a), &x) != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_TYPE, "scale must be a non-negative integer");
  bcl_ctxt_setScale (vm->ctx, (size_t) x);
  return builtin_finish_number (vm, bcl_bigdig2num (x));
}

typedef BclNumber (*ScBclUnaryKeep) (BclNumber);
typedef BclNumber (*ScBclBinaryKeep) (BclNumber, BclNumber);
typedef BclNumber (*ScBclTernaryKeep) (BclNumber, BclNumber, BclNumber);

static ScVmResult
builtin_unary (ScVm *vm, ScBclUnaryKeep fn)
{
  ScValue a;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  return builtin_finish_number (vm, fn (sc_vm_value_number (vm, a)));
}

static ScVmResult
builtin_binary (ScVm *vm, ScBclBinaryKeep fn)
{
  ScValue a, b;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK || sc_vm_arg (vm, 1, &b) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  return builtin_finish_number
    (vm, fn (sc_vm_value_number (vm, a), sc_vm_value_number (vm, b)));
}

static ScVmResult
builtin_ternary (ScVm *vm, ScBclTernaryKeep fn)
{
  ScValue a, b, c;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK || sc_vm_arg (vm, 1, &b) != SC_VM_OK
      || sc_vm_arg (vm, 2, &c) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  return builtin_finish_number
    (vm, fn (sc_vm_value_number (vm, a), sc_vm_value_number (vm, b),
             sc_vm_value_number (vm, c)));
}

#define DEFINE_UNARY(name, fn) static ScVmResult builtin_##name (ScVm *vm) { return builtin_unary (vm, fn); }
#define DEFINE_BINARY(name, fn) static ScVmResult builtin_##name (ScVm *vm) { return builtin_binary (vm, fn); }
#define DEFINE_TERNARY(name, fn) static ScVmResult builtin_##name (ScVm *vm) { return builtin_ternary (vm, fn); }
DEFINE_BINARY(add, bcl_add_keep)
DEFINE_BINARY(sub, bcl_sub_keep)
DEFINE_BINARY(mul, bcl_mul_keep)

static ScVmResult
builtin_div (ScVm *vm)
{
  ScValue a = SC_INVALID_VALUE, b = SC_INVALID_VALUE;
  bool zero = false;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK || sc_vm_arg (vm, 1, &b) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  if (sc_vm_value_is_zero (vm, b, &zero) != SC_VM_OK)
    return vm->error.code;
  if (zero)
    return vm_fail (vm, SC_ERR_DIVZERO, "division by zero");
  return builtin_finish_number
    (vm, bcl_div_keep (sc_vm_value_number (vm, a),
                       sc_vm_value_number (vm, b)));
}

static ScVmResult
builtin_mod (ScVm *vm)
{
  ScValue a = SC_INVALID_VALUE, b = SC_INVALID_VALUE;
  bool zero = false;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK || sc_vm_arg (vm, 1, &b) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  if (sc_vm_value_is_zero (vm, b, &zero) != SC_VM_OK)
    return vm->error.code;
  if (zero)
    return vm_fail (vm, SC_ERR_DIVZERO, "modulo by zero");
  return builtin_finish_number
    (vm, bcl_mod_keep (sc_vm_value_number (vm, a),
                       sc_vm_value_number (vm, b)));
}

DEFINE_BINARY(pow, bcl_pow_keep)
DEFINE_BINARY(lshift, bcl_lshift_keep)
DEFINE_BINARY(rshift, bcl_rshift_keep)
DEFINE_UNARY(sqrt, bcl_sqrt_keep)
DEFINE_TERNARY(modexp, bcl_modexp_keep)

static ScVmResult
builtin_neg (ScVm *vm)
{
  ScValue a;
  BclNumber n;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  n = bcl_dup (sc_vm_value_number (vm, a));
  if (bcl_err (n) != BCL_ERROR_NONE)
    return vm_fail (vm, SC_ERR_ARITHMETIC, "BCL duplicate error");
  bcl_num_setNeg (n, !bcl_num_neg (n));
  return builtin_finish_number (vm, n);
}

static ScVmResult
builtin_compare (ScVm *vm, int op)
{
  ScValue a, b;
  ssize_t cmp;
  bool result;
  if (sc_vm_arg (vm, 0, &a) != SC_VM_OK || sc_vm_arg (vm, 1, &b) != SC_VM_OK)
    return SC_ERR_ARG_COUNT;
  cmp = bcl_cmp (sc_vm_value_number (vm, a), sc_vm_value_number (vm, b));
  switch (op)
  {
    case 0: result = cmp == 0; break;
    case 1: result = cmp != 0; break;
    case 2: result = cmp < 0; break;
    case 3: result = cmp <= 0; break;
    case 4: result = cmp > 0; break;
    default: result = cmp >= 0; break;
  }
  return builtin_finish_number (vm, bcl_bigdig2num ((BclBigDig) result));
}
#define DEFINE_CMP(name, op) static ScVmResult builtin_##name (ScVm *vm) { return builtin_compare (vm, op); }
DEFINE_CMP(eq, 0)
DEFINE_CMP(ne, 1)
DEFINE_CMP(lt, 2)
DEFINE_CMP(le, 3)
DEFINE_CMP(gt, 4)
DEFINE_CMP(ge, 5)

#if SC_ENABLE_PERSISTENT_STATE
static ScVmResult
builtin_state_index (ScVm *vm, uint16_t arg, uint16_t *out)
{
  size_t index;
  ScVmResult rc = builtin_places_arg (vm, arg, &index);
  if (rc != SC_VM_OK)
    return rc;
  if (index > UINT16_MAX)
    return vm_fail (vm, SC_ERR_STATE_RANGE, "persistent state index out of range");
  *out = (uint16_t) index;
  return SC_VM_OK;
}

static ScVmResult
builtin_state_get (ScVm *vm)
{
  uint16_t index = 0;
  ScValue value;
  ScVmResult rc = builtin_state_index (vm, 0, &index);
  if (rc != SC_VM_OK) return rc;
  rc = sc_vm_state_get (vm, index, &value);
  if (rc != SC_VM_OK) return rc;
  rc = sc_vm_retain_value (vm, value);
  if (rc != SC_VM_OK) return rc;
  rc = sc_vm_set_result (vm, value);
  if (rc != SC_VM_OK) sc_vm_release_value (vm, value);
  return rc;
}

static ScVmResult
builtin_state_set (ScVm *vm)
{
  uint16_t index = 0;
  ScValue value;
  ScVmResult rc = builtin_state_index (vm, 0, &index);
  if (rc != SC_VM_OK) return rc;
  rc = sc_vm_arg (vm, 1, &value);
  if (rc != SC_VM_OK) return rc;
  rc = sc_vm_state_set (vm, index, value);
  if (rc != SC_VM_OK) return rc;
  rc = sc_vm_retain_value (vm, value);
  if (rc != SC_VM_OK) return rc;
  rc = sc_vm_set_result (vm, value);
  if (rc != SC_VM_OK) sc_vm_release_value (vm, value);
  return rc;
}
#endif

static ScVmResult
register_defaults (ScVm *vm)
{
  static const struct { const char *name; uint16_t argc; ScCFunc fn; } defs[] = {
    { "get_scale", 0, builtin_get_scale }, { "set_scale", 1, builtin_set_scale },
    { "add", 2, builtin_add }, { "sub", 2, builtin_sub },
    { "mul", 2, builtin_mul }, { "div", 2, builtin_div },
    { "mod", 2, builtin_mod }, { "pow", 2, builtin_pow },
    { "sqrt", 1, builtin_sqrt }, { "modexp", 3, builtin_modexp },
    { "lshift", 2, builtin_lshift }, { "rshift", 2, builtin_rshift },
    { "neg", 1, builtin_neg }, { "abs", 1, builtin_abs },
    { "num_scale", 1, builtin_num_scale }, { "length", 1, builtin_length },
    { "irand", 1, builtin_irand }, { "frand", 1, builtin_frand },
    { "ifrand", 2, builtin_ifrand },
    { "rand_seed", 1, builtin_rand_seed },
    { "rand_seed_get", 0, builtin_rand_seed_get },
    { "rand_reseed", 0, builtin_rand_reseed },
    { "eq", 2, builtin_eq }, { "ne", 2, builtin_ne },
    { "lt", 2, builtin_lt }, { "le", 2, builtin_le },
    { "gt", 2, builtin_gt }, { "ge", 2, builtin_ge },
#if SC_ENABLE_PERSISTENT_STATE
    { "state_get", 1, builtin_state_get }, { "state_set", 2, builtin_state_set }
#endif
  };
  size_t i;
  ScSymbolId id;
  for (i = 0; i < sizeof (defs) / sizeof (defs[0]); ++i)
    if (sc_vm_register_cfunc (vm, defs[i].name, defs[i].argc, defs[i].fn, &id)
        != SC_VM_OK)
      return vm->error.code;
  return SC_VM_OK;
}

/* ------------------------------ compiler -------------------------------- */

void
sc_compiler_init_slice (ScCompiler *c, ScVm *vm, ScSlice src)
{
  memset (c, 0, sizeof (*c));
  c->vm = vm;
  c->source = src;
  c->line = 1;
  c->column = 1;
  c->constant_prefix = '#';
}

void
sc_compiler_init (ScCompiler *c, ScVm *vm, const char *src, size_t len)
{
  sc_compiler_init_slice (c, vm, sc_slice_make (src, len));
}

static void sc_compiler_clear_proc_state (ScCompiler *c);

void
sc_compiler_reset_source_slice (ScCompiler *c, ScSlice src, size_t pos)
{
  c->source = src;
  c->pos = pos;
  c->line = 1;
  c->column = 1;
  c->compilation_id = 0;
  sc_compiler_clear_proc_state (c);
  memset (&c->error, 0, sizeof (c->error));
}

void
sc_compiler_reset_source (ScCompiler *c, const char *src, size_t len,
                          size_t pos)
{
  sc_compiler_reset_source_slice (c, sc_slice_make (src, len), pos);
}

static void
sc_compiler_clear_proc_state (ScCompiler *c)
{
  if (!c)
    return;
  if (c->scope.locals && c->scope.capacity)
    memset (c->scope.locals, 0, sizeof (*c->scope.locals) * c->scope.capacity);
  if (c->break_patches && c->break_patch_capacity)
    memset (c->break_patches, 0,
            sizeof (*c->break_patches) * c->break_patch_capacity);
  c->scope.count = 0;
  c->break_patch_count = 0;
  c->loop_depth = 0;
  c->parse_depth = 0;
  c->stack_depth = 0;
  c->max_stack = 0;
  c->current_proc = NULL;
  c->current_proc_symbol = SC_INVALID_SYMBOL;
  c->in_proc = false;
  c->has_return = false;
}

void
sc_compiler_destroy (ScCompiler *c)
{
  if (!c)
    return;
  free (c->scope.locals);
  free (c->break_patches);
  memset (c, 0, sizeof (*c));
}

const ScCompileError *
sc_compiler_error (const ScCompiler *c)
{
  return c ? &c->error : NULL;
}

ScCompileResult
sc_compiler_set_constant_prefix (ScCompiler *c, char prefix)
{
  if (!c)
    return SC_ERR_COMPILE_INVALID_PREFIX;
  if (!sc_numeric_prefix_valid (prefix))
    return sc_compile_error (c, SC_ERR_COMPILE_INVALID_PREFIX,
                             "invalid numeric literal prefix");
  c->constant_prefix = prefix;
  return SC_COMPILE_OK;
}

char
sc_compiler_get_constant_prefix (const ScCompiler *c)
{
  return c ? c->constant_prefix : '\0';
}


ScCompileResult
sc_compile_error_at (ScCompiler *c, size_t pos, uint32_t line, uint32_t col,
		     ScCompileResult code, const char *msg)
{
  c->error.code = code;
  c->error.source_pos = pos;
  c->error.line = line;
  c->error.column = col;
  snprintf (c->error.message, sizeof (c->error.message), "%s",
	    msg ? msg : "compile error");
  return code;
}

ScCompileResult
sc_compile_error (ScCompiler *c, ScCompileResult code, const char *msg)
{
  return sc_compile_error_at (c, c->pos, c->line, c->column, code, msg);
}

int
sc_peek (const ScCompiler *c)
{
  return c->pos < c->source.len ? (unsigned char) c->source.p[c->pos] : EOF;
}

int
sc_next (ScCompiler *c)
{
  int ch = sc_peek (c);
  if (ch == EOF)
    return EOF;
  c->pos++;
  if (ch == '\n')
  {
    c->line++;
    c->column = 1;
  }
  else
    c->column++;
  return ch;
}

bool
sc_match (ScCompiler *c, int e)
{
  if (sc_peek (c) != e)
    return false;
  sc_next (c);
  return true;
}

ScCompileResult
sc_expect (ScCompiler *c, int e, const char *m)
{
  return sc_match (c, e) ? SC_COMPILE_OK : sc_compile_error (c,
							     SC_ERR_COMPILE_SYNTAX,
							     m);
}

void
sc_skip_inline_whitespace (ScCompiler *c)
{
  int ch;
  while ((ch = sc_peek (c)) == ' ' || ch == '\t' || ch == '\r')
    sc_next (c);
}

void
sc_skip_command_separators (ScCompiler *c)
{
  int ch;
  while ((ch = sc_peek (c)) == ' ' || ch == '\t' || ch == '\r'
         || ch == '\n' || ch == ';')
    sc_next (c);
}

bool
sc_at_command_end (const ScCompiler *c, int term)
{
  int ch = sc_peek (c);
  return ch == EOF || ch == '\n' || ch == ';' || (term && ch == term);
}

ScCompileResult
sc_parse_bareword (ScCompiler *c, ScSlice *out)
{
  size_t s;
  int ch;
  sc_skip_inline_whitespace (c);
  s = c->pos;
  while ((ch = sc_peek (c)) != EOF && !isspace (ch) && ch != ';' && ch != '{'
	 && ch != '}' && ch != '[' && ch != ']' && ch != '$')
    sc_next (c);
  if (c->pos == s)
    return sc_compile_error (c, SC_ERR_COMPILE_SYNTAX, "expected word");
  out->p = c->source.p + s;
  out->len = c->pos - s;
  return SC_COMPILE_OK;
}

ScCompileResult
sc_parse_variable (ScCompiler *c, ScSlice *out)
{
  if (!sc_match (c, '$'))
    return sc_compile_error (c, SC_ERR_COMPILE_SYNTAX, "expected $");
  return sc_parse_bareword (c, out);
}

ScCompileResult
sc_find_local (const ScCompiler *c, ScSlice n, uint16_t *out)
{
  uint16_t i;
  for (i = 0; i < c->scope.count; i++)
    if (c->scope.locals[i].name.len == n.len
	&& !memcmp (c->scope.locals[i].name.p, n.p, n.len))
    {
      *out = c->scope.locals[i].slot;
      return SC_COMPILE_OK;
    }
  return SC_ERR_COMPILE_UNKNOWN_VAR;
}

static ScCompileResult
add_local (ScCompiler *c, ScSlice n, bool isarg, uint16_t *out)
{
  uint16_t slot;
  if (sc_find_local (c, n, &slot) == SC_COMPILE_OK)
    return sc_compile_error (c, SC_ERR_COMPILE_DUPLICATE_VAR,
			     "duplicate variable");
  {
    uint16_t i, locals = 0u;
    for (i = 0; i < c->scope.count; ++i)
      if (!c->scope.locals[i].is_arg) locals++;
    if (isarg && c->scope.count >= c->vm->limits.max_args)
      return sc_compile_error (c, SC_ERR_COMPILE_MAX_ARGS, "too many arguments");
    if (!isarg && locals >= c->vm->limits.max_locals)
      return sc_compile_error (c, SC_ERR_COMPILE_MAX_LOCALS, "too many locals");
    if (c->scope.count >= SC_MAX_SLOTS)
      return sc_compile_error (c, SC_ERR_COMPILE_MAX_LOCALS, "too many frame slots");
  }
  if (c->scope.count == c->scope.capacity)
  {
    uint16_t nc =
      c->scope.capacity ? c->scope.capacity * 2u : SC_COMPILER_LOCAL_CAP;
    ScLocal *np = realloc (c->scope.locals, sizeof (*np) * nc);
    if (!np)
      return sc_compile_error (c, SC_ERR_COMPILE_NO_MEM, "out of memory");
    c->scope.locals = np;
    c->scope.capacity = nc;
  }
  slot = c->scope.count;
  c->scope.locals[c->scope.count++] = (ScLocal)
  {
  n, slot, isarg};
  *out = slot;
  return SC_COMPILE_OK;
}

ScCompileResult
sc_find_or_create_local (ScCompiler *c, ScSlice n, uint16_t *out)
{
  if (sc_find_local (c, n, out) == SC_COMPILE_OK)
    return SC_COMPILE_OK;
  return add_local (c, n, false, out);
}

ScCompileResult
sc_add_argument (ScCompiler *c, ScSlice n, uint16_t *out)
{
  if (c->scope.count >= c->vm->limits.max_args)
    return sc_compile_error (c, SC_ERR_COMPILE_MAX_ARGS, "too many args");
  return add_local (c, n, true, out);
}

/* Parse and install a procedure argument list directly from the main source
 * cursor. No complete braced region is extracted or reparsed. */
static ScCompileResult
sc_compile_proc_arguments (ScCompiler *c)
{
  ScCompileResult r;

  if (!sc_match (c, '{'))
    return sc_compile_error (c, SC_ERR_COMPILE_MISSING_BRACE,
                             "expected { for argument list");

  for (;;)
  {
    ScSlice name;
    uint16_t slot;
    int ch;

    while ((ch = sc_peek (c)) == ' ' || ch == '\t' || ch == '\r'
           || ch == '\n')
      sc_next (c);

    if (sc_match (c, '}'))
      return SC_COMPILE_OK;
    if (sc_peek (c) == EOF)
      return sc_compile_error (c, SC_ERR_COMPILE_MISSING_BRACE,
                               "missing } in argument list");

    r = sc_parse_bareword (c, &name);
    if (r)
      return r;
    r = sc_add_argument (c, name, &slot);
    if (r)
      return r;
  }
}

ScCompileResult
sc_find_symbol (const ScCompiler *c, ScSlice n, ScSymbolId *out)
{
  return sc_symbol_lookup (&c->vm->symbols, n, out) ==
    SC_VM_OK ? SC_COMPILE_OK : SC_ERR_COMPILE_UNKNOWN_FUNC;
}

static ScCompileResult
sc_internal_proc_name (ScCompiler *c, ScSlice source_name, char **out,
                       size_t *out_len)
{
  char prefix[32];
  int prefix_len = 0;
  size_t len;
  char *z;
  if (!c || !out || !out_len || source_name.len == 0u)
    return sc_compile_error (c, SC_ERR_COMPILE_SYNTAX, "invalid proc name");
  if (isdigit ((unsigned char) source_name.p[0]))
    return sc_compile_error (c, SC_ERR_COMPILE_SYNTAX,
                             "procedure name may not start with a digit");
  if (source_name.p[0] == '_')
  {
    if (c->compilation_id == 0u)
    {
      if (c->vm->next_compilation_id == UINT32_MAX)
        return sc_compile_error (c, SC_ERR_COMPILE_INTERNAL,
                                 "compilation id exhausted");
      c->compilation_id = ++c->vm->next_compilation_id;
    }
    prefix_len = snprintf (prefix, sizeof (prefix), "%" PRIu32,
                           c->compilation_id);
    if (prefix_len < 0 || (size_t) prefix_len >= sizeof (prefix))
      return sc_compile_error (c, SC_ERR_COMPILE_INTERNAL,
                               "cannot format compilation id");
  }
  len = (size_t) prefix_len + source_name.len;
  if (len > SC_MAX_NAME_LEN)
    return sc_compile_error (c, SC_ERR_COMPILE_NAME_TOO_LONG,
                             "procedure name too long");
  z = malloc (len + 1u);
  if (!z)
    return sc_compile_error (c, SC_ERR_COMPILE_NO_MEM, "out of memory");
  if (prefix_len)
    memcpy (z, prefix, (size_t) prefix_len);
  memcpy (z + prefix_len, source_name.p, source_name.len);
  z[len] = 0;
  *out = z;
  *out_len = len;
  return SC_COMPILE_OK;
}

static ScVmResult
sc_vm_declare_source_placeholder (ScVm *vm, ScSlice name,
                                  uint32_t compilation_id, ScSymbolId *out)
{
  ScVmResult rc = sc_symbol_add (&vm->symbols, name, 0u, SC_SYM_EMPTY, out);
  if (rc != SC_VM_OK)
    return rc;
  vm->symbols.symbols[*out].decl_origin = SC_DECL_SOURCE;
  vm->symbols.symbols[*out].compilation_id = compilation_id;
  return SC_VM_OK;
}

static ScVmResult
sc_vm_declare_source_proc (ScVm *vm, ScSlice name,
                           uint16_t argc, uint32_t compilation_id,
                           ScSymbolId *out)
{
  ScSymbolId sid;
  if (sc_symbol_lookup (&vm->symbols, name, &sid) == SC_VM_OK)
  {
    ScSymbol *s = &vm->symbols.symbols[sid];
    if (s->kind != SC_SYM_EMPTY)
      return SC_ERR_INVALID_SYMBOL;
    if (s->decl_origin == SC_DECL_SOURCE &&
        s->compilation_id != compilation_id)
      return SC_ERR_INVALID_SYMBOL;
    s->argc = argc;
    *out = sid;
    return SC_VM_OK;
  }
  if (sc_symbol_add (&vm->symbols, name, argc, SC_SYM_EMPTY, out)
      != SC_VM_OK)
    return SC_ERR_NO_MEM;
  vm->symbols.symbols[*out].decl_origin = SC_DECL_SOURCE;
  vm->symbols.symbols[*out].compilation_id = compilation_id;
  return SC_VM_OK;
}

ScInstrIndex
sc_current_ip (const ScCompiler *c)
{
  return c->current_proc ? c->current_proc->code_count : 0;
}

ScCompileResult
sc_adjust_compile_stack (ScCompiler *c, int d)
{
  int n = (int) c->stack_depth + d;
  if (n < 0)
    return sc_compile_error (c, SC_ERR_COMPILE_INTERNAL,
			     "compile stack underflow");
  if (n > c->vm->limits.max_stack)
    return sc_compile_error (c, SC_ERR_COMPILE_MAX_STACK,
			     "compile stack overflow");
  c->stack_depth = (uint16_t) n;
  if (c->stack_depth > c->max_stack)
    c->max_stack = c->stack_depth;
  return SC_COMPILE_OK;
}

ScCompileResult
sc_emit_instruction (ScCompiler *c, ScVmOpCode op, ScJumpHint hint,
		     uint16_t a, uint32_t b, ScInstrIndex *out)
{
  ScProc *p = c->current_proc;
  uint32_t cap = p->code_capacity;
  if (!grow_array
      ((void **) &p->code, sizeof (*p->code), &cap, p->code_count + 1u,
       c->vm->limits.max_bytecode_bytes / sizeof (ScInstr)))
    return sc_compile_error (c, SC_ERR_COMPILE_MAX_BYTECODE,
			     "bytecode allocation/limit");
  p->code_capacity = cap;
  if (out)
    *out = p->code_count;
  p->code[p->code_count++] = (ScInstr)
  {
  (uint8_t) op, (uint8_t) hint, a, b};
  return SC_COMPILE_OK;
}

ScCompileResult
sc_emit_const (ScCompiler *c, ScValue v)
{
  ScCompileResult r =
    sc_emit_instruction (c, SC_OP_CONST, SC_JUMP_PLAIN, 0, v, NULL);
  return r ? r : sc_adjust_compile_stack (c, 1);
}

ScCompileResult
sc_emit_load (ScCompiler *c, uint16_t s)
{
  ScCompileResult r =
    sc_emit_instruction (c, SC_OP_LOAD, SC_JUMP_PLAIN, s, 0, NULL);
  return r ? r : sc_adjust_compile_stack (c, 1);
}

ScCompileResult
sc_emit_store (ScCompiler *c, uint16_t s)
{
  ScCompileResult r =
    sc_emit_instruction (c, SC_OP_STORE, SC_JUMP_PLAIN, s, 0, NULL);
  return r ? r : sc_adjust_compile_stack (c, -1);
}

ScCompileResult
sc_emit_call (ScCompiler *c, ScSymbolId id, uint16_t argc)
{
  ScCompileResult r =
    sc_emit_instruction (c, SC_OP_CALL, SC_JUMP_PLAIN, argc, id, NULL);
  return r ? r : sc_adjust_compile_stack (c, 1 - (int) argc);
} ScCompileResult

sc_emit_pop (ScCompiler *c)
{
  ScCompileResult r =
    sc_emit_instruction (c, SC_OP_POP, SC_JUMP_PLAIN, 0, 0, NULL);
  return r ? r : sc_adjust_compile_stack (c, -1);
}

ScCompileResult
sc_emit_ret (ScCompiler *c)
{
  ScCompileResult r =
    sc_emit_instruction (c, SC_OP_RET, SC_JUMP_PLAIN, 0, 0, NULL);
  return r ? r : sc_adjust_compile_stack (c, -1);
}

ScCompileResult
sc_emit_jump_placeholder (ScCompiler *c, ScVmOpCode op, ScJumpHint h,
			  ScInstrIndex *out)
{
  return sc_emit_instruction (c, op, h, 0, SC_INVALID_INDEX, out);
}

ScCompileResult
sc_patch_jump (ScCompiler *c, ScInstrIndex i, ScInstrIndex t)
{
  if (i >= c->current_proc->code_count)
    return sc_compile_error (c, SC_ERR_COMPILE_INTERNAL, "bad patch");
  c->current_proc->code[i].b = t;
  return SC_COMPILE_OK;
}

ScCompileResult
sc_push_loop (ScCompiler *c, ScInstrIndex s)
{
  if (c->loop_depth >= c->vm->limits.max_loop_depth)
    return sc_compile_error (c, SC_ERR_COMPILE_MAX_LOOP_DEPTH,
			     "loop nesting too deep");
  c->loops[c->loop_depth++] = (ScLoopCtx)
  {
  s, c->break_patch_count};
  return SC_COMPILE_OK;
}

ScCompileResult
sc_record_break (ScCompiler *c, ScInstrIndex i)
{
  uint32_t cap = c->break_patch_capacity;
  if (!c->loop_depth)
    return sc_compile_error (c, SC_ERR_COMPILE_BREAK_OUTSIDE_LOOP,
			     "break outside loop");
  if (c->break_patch_count -
      c->loops[c->loop_depth - 1u].first_break_patch >=
      SC_MAX_BREAKS_PER_LOOP)
    return sc_compile_error (c, SC_ERR_COMPILE_MAX_BREAKS,
                             "too many breaks in loop");
  if (!grow_array
      ((void **) &c->break_patches, sizeof (*c->break_patches), &cap,
       c->break_patch_count + 1u, SC_MAX_LOOP_DEPTH * SC_MAX_BREAKS_PER_LOOP))
    return sc_compile_error (c, SC_ERR_COMPILE_NO_MEM,
			     "break patch allocation");
  c->break_patch_capacity = cap;
  c->break_patches[c->break_patch_count++] = (ScPatch)
  {
  i};
  return SC_COMPILE_OK;
}

ScCompileResult
sc_finish_loop (ScCompiler *c, ScInstrIndex end)
{
  ScLoopCtx *l;
  uint32_t i;
  if (!c->loop_depth)
    return SC_ERR_COMPILE_INTERNAL;
  l = &c->loops[c->loop_depth - 1u];
  for (i = l->first_break_patch; i < c->break_patch_count; i++)
    sc_patch_jump (c, c->break_patches[i].instruction, end);
  c->break_patch_count = l->first_break_patch;
  c->loop_depth--;
  return SC_COMPILE_OK;
}

static bool
sc_numeric_literal_valid (ScSlice lit, char prefix)
{
  size_t i = 0;
  bool radix = false;
  bool digit = false;

  if (prefix != '\0' && lit.len > 0 && lit.p[0] == prefix)
    i = 1;
  if (i < lit.len && lit.p[i] == '-')
    i++;
  if (i == lit.len)
    return false;

  for (; i < lit.len; ++i)
  {
    unsigned char ch = (unsigned char) lit.p[i];
    if (ch == '.')
    {
      if (radix)
        return false;
      radix = true;
      continue;
    }
    if ((ch >= (unsigned char) '0' && ch <= (unsigned char) '9') ||
        (ch >= (unsigned char) 'A' && ch <= (unsigned char) 'Z'))
    {
      digit = true;
      continue;
    }
    return false;
  }
  return digit;
}

ScCompileResult
sc_intern_constant (ScCompiler *c, ScSlice lit, ScValue *out)
{
  char *z;
  BclNumber n;
  ScVmResult vr;
  if (lit.len >= SC_MAX_SOURCE_LEN)
    return sc_compile_error (c, SC_ERR_COMPILE_INVALID_CONSTANT,
			     "constant too long");
  if (!sc_numeric_literal_valid (lit, c->constant_prefix))
    return sc_compile_error (c, SC_ERR_COMPILE_INVALID_CONSTANT,
			     "invalid numeric constant");
  z = malloc (lit.len + 1u);
  if (!z)
    return SC_ERR_COMPILE_NO_MEM;
  if (c->constant_prefix != '\0' && lit.len > 1
      && lit.p[0] == c->constant_prefix)
  {
    memcpy (z, lit.p + 1, lit.len - 1);
    z[lit.len - 1] = '\0';
  }
  else
  {
    memcpy (z, lit.p, lit.len);
    z[lit.len] = '\0';
  }
  n = bcl_parse (z);
  free (z);
  if (bcl_err (n) != BCL_ERROR_NONE)
    return sc_compile_error (c, SC_ERR_COMPILE_INVALID_CONSTANT,
			     "invalid numeric constant");
  vr = sc_vm_new_permanent_value (c->vm, n, out);
  return vr == SC_VM_OK ? SC_COMPILE_OK : sc_compile_error (c,
							    SC_ERR_COMPILE_NO_MEM,
							    "constant slot allocation");
}

ScCompileResult
sc_compile_constant (ScCompiler *c, ScSlice l)
{
  ScValue v = SC_INVALID_VALUE;
  ScCompileResult r = sc_intern_constant (c, l, &v);
  return r ? r : sc_emit_const (c, v);
}

ScCompileResult
sc_compile_variable (ScCompiler *c)
{
  ScSlice n;
  uint16_t s;
  ScCompileResult r = sc_parse_variable (c, &n);
  if (r)
    return r;
  if (sc_find_local (c, n, &s) != SC_COMPILE_OK)
    return sc_compile_error (c, SC_ERR_COMPILE_UNKNOWN_VAR,
			     "unknown variable");
  return sc_emit_load (c, s);
}

ScCompileResult
sc_compile_expression (ScCompiler *c)
{
  int ch;
  ScSlice w;
  sc_skip_inline_whitespace (c);
  ch = sc_peek (c);
  if (ch == '$')
    return sc_compile_variable (c);
  if (ch == '[')
    return sc_compile_bracketed (c);
  if (ch == EOF || ch == '{' || ch == '}' || ch == ']')
    return sc_compile_error (c, SC_ERR_COMPILE_INVALID_EXPR,
			     "expected expression");
  if (sc_parse_bareword (c, &w))
    return c->error.code;
  return sc_compile_constant (c, w);
}

ScCompileResult
sc_compile_call (ScCompiler *c, ScSlice name, int term)
{
  ScSymbolId sid;
  uint16_t argc = 0;
  ScCompileResult r;
  char *internal_name = NULL;
  size_t internal_len = 0;

  r = sc_internal_proc_name (c, name, &internal_name, &internal_len);
  if (r)
    return r;

  while (1)
  {
    sc_skip_inline_whitespace (c);
    if (sc_at_command_end (c, term))
      break;
    r = sc_compile_expression (c);
    if (r)
    {
      free (internal_name);
      return r;
    }
    if (argc == UINT16_MAX)
    {
      free (internal_name);
      return sc_compile_error (c, SC_ERR_COMPILE_MAX_ARGS,
                               "too many call arguments");
    }
    argc++;
  }

  if (sc_symbol_lookup (&c->vm->symbols, sc_slice_make (internal_name, internal_len), &sid)
      != SC_VM_OK)
  {
    if (sc_vm_declare_source_placeholder (c->vm, sc_slice_make (internal_name, internal_len),
                                          c->compilation_id, &sid) != SC_VM_OK)
    {
      free (internal_name);
      return sc_compile_error (c, SC_ERR_COMPILE_MAX_SYMBOLS,
                               "cannot declare function placeholder");
    }
  }
  free (internal_name);
  return sc_emit_call (c, sid, argc);
}

ScCompileResult
sc_compile_bracketed (ScCompiler *c)
{
  ScSlice name;
  ScCompileResult r;
  if (c->parse_depth >= c->vm->limits.max_parse_depth)
    return sc_compile_error (c, SC_ERR_COMPILE_MAX_NESTING,
                             "parser nesting too deep");
  c->parse_depth++;
  if (sc_expect (c, '[', "expected ["))
  {
    r = c->error.code;
    goto done;
  }
  sc_skip_inline_whitespace (c);
  r = sc_parse_bareword (c, &name);
  if (r)
    goto done;
  r = sc_compile_call (c, name, ']');
  if (r)
    goto done;
  sc_skip_inline_whitespace (c);
  r = sc_expect (c, ']', "missing ]");
done:
  c->parse_depth--;
  return r;
}

ScCompileResult
sc_compile_set (ScCompiler *c)
{
  ScSlice n;
  uint16_t s;
  ScCompileResult r;
  sc_skip_inline_whitespace (c);
  r = sc_parse_bareword (c, &n);
  if (r)
    return r;
  r = sc_find_or_create_local (c, n, &s);
  if (r)
    return r;
  r = sc_compile_expression (c);
  if (r)
    return r;
  return sc_emit_store (c, s);
}

ScCompileResult
sc_compile_return (ScCompiler *c)
{
  ScCompileResult r;
  if (!c->in_proc)
    return sc_compile_error (c, SC_ERR_COMPILE_RETURN_OUTSIDE_PROC,
			     "return outside proc");
  r = sc_compile_expression (c);
  if (r)
    return r;
  c->has_return = true;
  return sc_emit_ret (c);
}

ScCompileResult
sc_compile_break (ScCompiler *c)
{
  ScInstrIndex i;
  ScCompileResult r =
    sc_emit_jump_placeholder (c, SC_OP_JMP, SC_JUMP_LOOP_BREAK, &i);
  return r ? r : sc_record_break (c, i);
}

ScCompileResult
sc_compile_braced_body (ScCompiler *c)
{
  ScCompileResult r;
  if (c->parse_depth >= c->vm->limits.max_parse_depth)
    return sc_compile_error (c, SC_ERR_COMPILE_MAX_NESTING,
                             "parser nesting too deep");
  c->parse_depth++;
  if (sc_expect (c, '{', "expected {"))
  {
    r = c->error.code;
    goto done;
  }
  r = sc_compile_body (c, '}');
  if (r)
    goto done;
  r = sc_expect (c, '}', "missing }");
done:
  c->parse_depth--;
  return r;
}

ScCompileResult
sc_compile_if (ScCompiler *c)
{
  ScInstrIndex jf, je;
  ScCompileResult r;
  uint16_t base = c->stack_depth;
  r = sc_compile_expression (c);
  if (r)
    return r;
  r = sc_emit_jump_placeholder (c, SC_OP_JZ, SC_JUMP_IF_FALSE, &jf);
  if (r)
    return r;
  r = sc_adjust_compile_stack (c, -1);
  if (r)
    return r;
  sc_skip_inline_whitespace (c);
  r = sc_compile_braced_body (c);
  if (r)
    return r;
  c->stack_depth = base;
  sc_skip_inline_whitespace (c);
  if (sc_peek (c) != '{')
    return sc_patch_jump (c, jf, sc_current_ip (c));
  r = sc_emit_jump_placeholder (c, SC_OP_JMP, SC_JUMP_IF_END, &je);
  if (r)
    return r;
  sc_patch_jump (c, jf, sc_current_ip (c));
  r = sc_compile_braced_body (c);
  if (r)
    return r;
  c->stack_depth = base;
  return sc_patch_jump (c, je, sc_current_ip (c));
}

ScCompileResult
sc_compile_loop (ScCompiler *c)
{
  ScInstrIndex start = sc_current_ip (c);
  ScCompileResult r = sc_push_loop (c, start);
  if (r)
    return r;
  sc_skip_inline_whitespace (c);
  r = sc_compile_braced_body (c);
  if (r)
    return r;
  r = sc_emit_instruction (c, SC_OP_JMP, SC_JUMP_LOOP_BACK, 0, start, NULL);
  if (r)
    return r;
  return sc_finish_loop (c, sc_current_ip (c));
}

ScCompileResult
sc_compile_command (ScCompiler *c)
{
  ScSlice name;
  ScCompileResult r = sc_parse_bareword (c, &name);
  if (r)
    return r;
  if (sc_slice_equal_cstr (name, "set"))
    r = sc_compile_set (c);
  else if (sc_slice_equal_cstr (name, "if"))
    r = sc_compile_if (c);
  else if (sc_slice_equal_cstr (name, "loop"))
    r = sc_compile_loop (c);
  else if (sc_slice_equal_cstr (name, "break"))
    r = sc_compile_break (c);
  else if (sc_slice_equal_cstr (name, "return"))
    r = sc_compile_return (c);
  else
  {
    r = sc_compile_call (c, name, 0);
    if (!r)
      r = sc_emit_pop (c);
  }
  return r;
}

ScCompileResult
sc_compile_body (ScCompiler *c, int term)
{
  ScCompileResult r;
  for (;;)
  {
    sc_skip_command_separators (c);
    if (sc_peek (c) == term && term)
      return SC_COMPILE_OK;
    if (sc_peek (c) == EOF)
      return term ? sc_compile_error (c, SC_ERR_COMPILE_UNEXPECTED_EOF,
				      "unexpected eof") : SC_COMPILE_OK;
    r = sc_compile_command (c);
    if (r)
      return r;
    sc_skip_inline_whitespace (c);
    if (sc_peek (c) == ';' || sc_peek (c) == '\n')
      sc_next (c);
    else if (!(term && sc_peek (c) == term) && sc_peek (c) != EOF)
      return sc_compile_error (c, SC_ERR_COMPILE_SYNTAX,
			       "expected command separator");
  }
}

ScCompileResult
sc_compile_proc (ScCompiler *c, ScProcId *out)
{
  ScSlice kw, name;
  ScProc temp;
  ScSymbolId sid;
  ScCompileResult r;
  ScProcId pid;
  sc_skip_command_separators (c);
  r = sc_parse_bareword (c, &kw);
  if (r)
    return r;
  if (!sc_slice_equal_cstr (kw, "proc"))
    return sc_compile_error (c, SC_ERR_COMPILE_INVALID_PROC_DEF,
			     "expected proc");
  sc_skip_inline_whitespace (c);
  r = sc_parse_bareword (c, &name);
  if (r)
    return r;
  memset (&temp, 0, sizeof (temp));
  temp.source_line = c->line;
  c->current_proc = &temp;
  c->scope.count = 0;
  c->stack_depth = c->max_stack = 0;
  c->parse_depth = 0;
  c->loop_depth = 0;
  c->break_patch_count = 0;
  c->has_return = false;
  c->in_proc = true;
  sc_skip_inline_whitespace (c);
  r = sc_compile_proc_arguments (c);
  if (r)
    goto fail;
  temp.argc = c->scope.count;
  {
    char *z = NULL;
    size_t zlen = 0;
    r = sc_internal_proc_name (c, name, &z, &zlen);
    if (r)
      goto fail;
    if (sc_vm_declare_source_proc (c->vm, sc_slice_make (z, zlen), temp.argc,
                                   c->compilation_id, &sid) != SC_VM_OK)
    {
      free (z);
      r =
	sc_compile_error (c, SC_ERR_COMPILE_DUPLICATE_PROC, "duplicate proc");
      goto fail;
    }
    free (z);
  }
  c->current_proc_symbol = sid;
  temp.symbol_id = sid;
  sc_skip_inline_whitespace (c);
  r = sc_compile_braced_body (c);
  if (r)
    goto fail;
  if (!c->has_return)
  {
    r =
      sc_compile_error (c, SC_ERR_COMPILE_NO_RETURN_VALUE,
			"procedure needs return");
    goto fail;
  }
  temp.slot_count = c->scope.count;
  temp.max_stack = c->max_stack;
  if (sc_vm_install_proc (c->vm, sid, &temp, &pid) != SC_VM_OK)
  {
    r = sc_compile_error (c, SC_ERR_COMPILE_NO_MEM, "install proc failed");
    goto fail2;
  }
  if (out)
    *out = pid;
  sc_compiler_clear_proc_state (c);
  return SC_COMPILE_OK;
fail:
  sc_proc_destroy (&temp);
  sc_compiler_clear_proc_state (c);
  return r;
fail2:
  sc_proc_destroy (&temp);
  sc_compiler_clear_proc_state (c);
  return r;
}

ScCompileResult
sc_compile_source (ScCompiler *c, uint32_t *outn)
{
  uint32_t n = 0, i;
  ScCompileResult r;
  if (!c || !c->source.p || !c->vm)
    return SC_ERR_COMPILE_INTERNAL;
  if (c->source.len > c->vm->limits.max_source_len)
    return sc_compile_error (c, SC_ERR_COMPILE_MAX_BYTECODE,
                             "source length limit exceeded");
  if (memchr (c->source.p, '\0', c->source.len) != NULL)
    return sc_compile_error (c, SC_ERR_COMPILE_SYNTAX,
			     "embedded NUL byte in source");
  if (c->vm->next_compilation_id == UINT32_MAX)
    return sc_compile_error (c, SC_ERR_COMPILE_INTERNAL,
                             "compilation id exhausted");
  c->compilation_id = ++c->vm->next_compilation_id;
  while (1)
  {
    sc_skip_command_separators (c);
    if (sc_peek (c) == EOF)
      break;
    r = sc_compile_proc (c, NULL);
    if (r)
      return r;
    n++;
  }
  for (i = 0; i < c->vm->symbols.count; ++i)
  {
    const ScSymbol *s = &c->vm->symbols.symbols[i];
    if (s->kind == SC_SYM_EMPTY && s->decl_origin == SC_DECL_SOURCE &&
        s->compilation_id == c->compilation_id)
    {
      const char *display = s->name;
      while (isdigit ((unsigned char) *display))
        display++;
      char message[SC_MAX_ERROR_MSG_LEN];
      snprintf (message, sizeof (message), "undefined function \"%s\"",
                display);
      return sc_compile_error (c, SC_ERR_COMPILE_UNKNOWN_FUNC, message);
    }
  }
  if (outn)
    *outn = n;
  return SC_COMPILE_OK;
}


#if SC_EMBEDDED_LIBRARY_LEVEL >= 1
static ScCompileResult
sc_vm_compile_embedded_library (ScVm *vm, const unsigned char *source,
                                size_t source_len, ScCompileError *out_error)
{
  ScCompiler compiler;
  ScCompileResult result;
  uint32_t proc_count = 0u;

  sc_compiler_init (&compiler, vm, (const char *) source, source_len);
  result = sc_compile_source (&compiler, &proc_count);
  if (out_error)
  {
    if (result == SC_COMPILE_OK)
      memset (out_error, 0, sizeof (*out_error));
    else
      *out_error = compiler.error;
  }
  sc_compiler_destroy (&compiler);
  return result;
}
#endif

ScLibraryLevel
sc_vm_embedded_library_level (void)
{
  return (ScLibraryLevel) SC_EMBEDDED_LIBRARY_LEVEL;
}

ScLibraryLevel
sc_vm_loaded_library_level (const ScVm *vm)
{
  return vm ? vm->loaded_library_level : SC_LIBRARY_NONE;
}

ScCompileResult
sc_vm_load_libraries (ScVm *vm, ScLibraryLevel level,
                      ScCompileError *out_error)
{
  ScCompileResult result = SC_COMPILE_OK;

  SC_UNUSED (result);
  if (out_error)
    memset (out_error, 0, sizeof (*out_error));
  if (!vm || level < SC_LIBRARY_NONE || level > SC_LIBRARY_1_2_3)
  {
    if (out_error)
    {
      out_error->code = SC_ERR_COMPILE_INVALID_LIBRARY_LEVEL;
      snprintf (out_error->message, sizeof (out_error->message),
                "invalid embedded library level");
    }
    return SC_ERR_COMPILE_INVALID_LIBRARY_LEVEL;
  }
  if (level > sc_vm_embedded_library_level ())
  {
    if (out_error)
    {
      out_error->code = SC_ERR_COMPILE_LIBRARY_UNAVAILABLE;
      snprintf (out_error->message, sizeof (out_error->message),
                "requested embedded library level is not available in this build");
    }
    return SC_ERR_COMPILE_LIBRARY_UNAVAILABLE;
  }
  if (level <= vm->loaded_library_level)
    return SC_COMPILE_OK;

#if SC_EMBEDDED_LIBRARY_LEVEL >= 1
  if (level >= SC_LIBRARY_1 && vm->loaded_library_level < SC_LIBRARY_1)
  {
    result = sc_vm_compile_embedded_library (
        vm, sc_embedded_library_1_source, sc_embedded_library_1_source_len,
        out_error);
    if (result != SC_COMPILE_OK)
      return result;
    vm->loaded_library_level = SC_LIBRARY_1;
  }
#endif
#if SC_EMBEDDED_LIBRARY_LEVEL >= 2
  if (level >= SC_LIBRARY_1_2 && vm->loaded_library_level < SC_LIBRARY_1_2)
  {
    result = sc_vm_compile_embedded_library (
        vm, sc_embedded_library_2_source, sc_embedded_library_2_source_len,
        out_error);
    if (result != SC_COMPILE_OK)
      return result;
    vm->loaded_library_level = SC_LIBRARY_1_2;
  }
#endif
#if SC_EMBEDDED_LIBRARY_LEVEL >= 3
  if (level >= SC_LIBRARY_1_2_3 &&
      vm->loaded_library_level < SC_LIBRARY_1_2_3)
  {
    result = sc_vm_compile_embedded_library (
        vm, sc_embedded_library_3_source, sc_embedded_library_3_source_len,
        out_error);
    if (result != SC_COMPILE_OK)
      return result;
    vm->loaded_library_level = SC_LIBRARY_1_2_3;
  }
#endif
  return SC_COMPILE_OK;
}

ScCompileResult
sc_verify_proc (const ScVm *vm, const ScProc *p, ScCompileError *out)
{
  uint32_t i, qhead = 0, qtail = 0;
  int max = 0;
  int *depths = NULL;
  uint32_t *queue = NULL;
  const int unseen = -2147483647;

  if (!vm || !p || p->slot_count < p->argc ||
      (p->code_count != 0u && p->code == NULL))
    goto bad_no_index;

  depths = malloc(sizeof(*depths) * (p->code_count + 1u));
  queue = malloc(sizeof(*queue) * (p->code_count + 1u));
  if (!depths || !queue) goto oom;
  for (i = 0; i <= p->code_count; ++i) depths[i] = unseen;
  depths[0] = 0;
  queue[qtail++] = 0u;

  while (qhead < qtail)
  {
    uint32_t ip = queue[qhead++];
    ScInstr in;
    int before, after;
    uint32_t succ[2];
    unsigned nsucc = 0u, k;

    if (ip >= p->code_count) { i = ip; goto bad; }
    in = p->code[ip];
    before = depths[ip];
    after = before;
    i = ip;

    switch (in.op)
    {
    case SC_OP_CONST:
      if (!sc_vm_value_valid(vm, in.b)) goto bad;
      after++;
      succ[nsucc++] = ip + 1u;
      break;
    case SC_OP_LOAD:
      if (in.a >= p->slot_count) goto bad;
      after++;
      succ[nsucc++] = ip + 1u;
      break;
    case SC_OP_STORE:
      if (in.a >= p->slot_count || before < 1) goto bad;
      after--;
      succ[nsucc++] = ip + 1u;
      break;
    case SC_OP_CALL:
      if (in.b >= vm->symbols.count ||
          in.a != vm->symbols.symbols[in.b].argc || before < (int)in.a)
        goto bad;
      after = before - (int)in.a + 1;
      succ[nsucc++] = ip + 1u;
      break;
    case SC_OP_JMP:
      if (in.b > p->code_count) goto bad;
      succ[nsucc++] = in.b;
      break;
    case SC_OP_JZ:
    case SC_OP_JNZ:
      if (in.b > p->code_count || before < 1) goto bad;
      after--;
      succ[nsucc++] = in.b;
      succ[nsucc++] = ip + 1u;
      break;
    case SC_OP_POP:
      if (before < 1) goto bad;
      after--;
      succ[nsucc++] = ip + 1u;
      break;
    case SC_OP_RET:
      if (before < 1) goto bad;
      after--;
      break;
    default:
      goto bad;
    }

    if (after > max) max = after;
    if (after < 0 || after > (int)p->max_stack) goto bad;
    for (k = 0; k < nsucc; ++k)
    {
      uint32_t target = succ[k];
      if (target > p->code_count) goto bad;
      if (depths[target] == unseen)
      {
        depths[target] = after;
        queue[qtail++] = target;
      }
      else if (depths[target] != after)
        goto bad;
    }
  }

  if (depths[p->code_count] != unseen) { i = p->code_count; goto bad; }
  free(depths);
  free(queue);
  return SC_COMPILE_OK;

oom:
  free(depths);
  free(queue);
  if (out)
  {
    memset(out, 0, sizeof(*out));
    out->code = SC_ERR_COMPILE_NO_MEM;
    snprintf(out->message, sizeof(out->message), "bytecode verifier allocation failed");
  }
  return SC_ERR_COMPILE_NO_MEM;

bad_no_index:
  i = 0u;
bad:
  free(depths);
  free(queue);
  if (out)
  {
    memset(out, 0, sizeof(*out));
    out->code = SC_ERR_COMPILE_INTERNAL;
    snprintf(out->message, sizeof(out->message),
             "invalid bytecode at instruction %u", i);
  }
  return SC_ERR_COMPILE_INTERNAL;
}

ScVmResult
sc_disassemble_proc (const ScVm *vm, const ScProc *p, ScDisasmWriteFn wr,
		     void *ctx)
{
  uint32_t i;
  char b[160];
  for (i = 0; i < p->code_count; i++)
  {
    ScInstr in = p->code[i];
    const char *op = "?";
    switch (in.op)
    {
    case SC_OP_CONST:
      op = "CONST";
      break;
    case SC_OP_LOAD:
      op = "LOAD";
      break;
    case SC_OP_STORE:
      op = "STORE";
      break;
    case SC_OP_CALL:
      op = "CALL";
      break;
    case SC_OP_JMP:
      op = "JMP";
      break;
    case SC_OP_JZ:
      op = "JZ";
      break;
    case SC_OP_JNZ:
      op = "JNZ";
      break;
    case SC_OP_RET:
      op = "RET";
      break;
    case SC_OP_POP:
      op = "POP";
      break;
    }
    if (in.op == SC_OP_CALL && in.b < vm->symbols.count)
      snprintf (b, sizeof (b), "%u: %s %s %u\n", i, op,
		vm->symbols.symbols[in.b].name, in.a);
    else
      snprintf (b, sizeof (b), "%u: %s %u %u ; hint=%u\n", i, op, in.a, in.b,
		in.hint);
    if (!wr (ctx, b, strlen (b)))
      return SC_ERR_RUNTIME;
  }
  return SC_VM_OK;
}

bool
sc_vm_error_has_disassembly (const ScVm *vm)
{
  const ScVmError *e;
  const ScSymbol *s;
  if (!vm)
    return false;
  e = &vm->error;
  if (e->code == SC_VM_OK || e->symbol_id == SC_INVALID_SYMBOL
      || e->ip == SC_INVALID_INDEX || e->symbol_id >= vm->symbols.count)
    return false;
  s = &vm->symbols.symbols[e->symbol_id];
  return s->kind == SC_SYM_PROC && s->target.proc_id < vm->proc_count
         && e->ip < vm->procs[s->target.proc_id].code_count;
}

ScVmResult
sc_vm_disassemble_error (const ScVm *vm, ScDisasmWriteFn wr, void *ctx)
{
  const ScSymbol *s;
  if (!wr || !sc_vm_error_has_disassembly (vm))
    return SC_ERR_RUNTIME;
  s = &vm->symbols.symbols[vm->error.symbol_id];
  return sc_disassemble_proc (vm, &vm->procs[s->target.proc_id], wr, ctx);
}

/* Optional convenience: call once immediately after sc_vm_init(). */
ScVmResult
sc_vm_register_core_builtins (ScVm *vm)
{
  return register_defaults (vm);
}
