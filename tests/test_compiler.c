#include "compiler.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(BclBigDig) * 8u == BC_LONG_BIT,
               "BclBigDig ABI does not match BC_LONG_BIT");

static ScValue parse_value(ScVm *vm, const char *z)
{
    BclNumber n = bcl_parse(z);
    ScValue v;
    assert(bcl_err(n) == BCL_ERROR_NONE);
    assert(sc_vm_new_value(vm, n, &v) == SC_VM_OK);
    return v;
}

static void assert_value(ScVm *vm, ScValue v, const char *expected)
{
    BclNumber want = bcl_parse(expected);
    BclNumber got = sc_vm_value_number(vm, v);
    assert(bcl_err(want) == BCL_ERROR_NONE);
    if (bcl_cmp(got, want) != 0 || bcl_num_scale(got) != bcl_num_scale(want)) {
        char *actual = bcl_string(got);
        fprintf(stderr, "expected numeric value %s (scale %zu), got %s (scale %zu)\n",
                expected, bcl_num_scale(want), actual, bcl_num_scale(got));
        free(actual);
        bcl_num_free(want);
        abort();
    }
    bcl_num_free(want);
}

static void call_expect(ScVm *vm, const char *name, const char **argv,
                        uint16_t argc, const char *expected)
{
    ScValue args[3] = { 0, 0, 0 }, out;
    uint16_t i;
    assert(argc <= 3);
    for (i = 0; i < argc; ++i) args[i] = parse_value(vm, argv[i]);
    assert(sc_vm_call_name(vm, name, strlen(name), args, argc, &out) == SC_VM_OK);
    assert_value(vm, out, expected);
    assert(sc_vm_release_value(vm, out) == SC_VM_OK);
    for (i = 0; i < argc; ++i) assert(sc_vm_release_value(vm, args[i]) == SC_VM_OK);
}

static void call_math_expect(ScVm *vm, const char *name, const char **argv,
                             uint16_t argc, const char *expected)
{
    call_expect(vm, name, argv, argc, expected);
}

static void call_expect_numeric(ScVm *vm, const char *name, const char **argv,
                                uint16_t argc, const char *expected)
{
    ScValue args[3] = { 0, 0, 0 }, out;
    BclNumber want = bcl_parse(expected);
    uint16_t i;
    assert(argc <= 3);
    assert(bcl_err(want) == BCL_ERROR_NONE);
    for (i = 0; i < argc; ++i) args[i] = parse_value(vm, argv[i]);
    assert(sc_vm_call_name(vm, name, strlen(name), args, argc, &out) == SC_VM_OK);
    assert(bcl_cmp(sc_vm_value_number(vm, out), want) == 0);
    bcl_num_free(want);
    assert(sc_vm_release_value(vm, out) == SC_VM_OK);
    for (i = 0; i < argc; ++i) assert(sc_vm_release_value(vm, args[i]) == SC_VM_OK);
}

static ScValue call_values(ScVm *vm, const char *name, const char **argv,
                           uint16_t argc)
{
    ScValue args[3] = { 0, 0, 0 }, out;
    uint16_t i;
    assert(argc <= 3);
    for (i = 0; i < argc; ++i) args[i] = parse_value(vm, argv[i]);
    assert(sc_vm_call_name(vm, name, strlen(name), args, argc, &out) == SC_VM_OK);
    for (i = 0; i < argc; ++i) assert(sc_vm_release_value(vm, args[i]) == SC_VM_OK);
    return out;
}

static void assert_between(ScVm *vm, ScValue value, const char *lo,
                           const char *hi, int hi_inclusive)
{
    BclNumber nlo = bcl_parse(lo);
    BclNumber nhi = bcl_parse(hi);
    BclNumber got = sc_vm_value_number(vm, value);
    assert(bcl_err(nlo) == BCL_ERROR_NONE);
    assert(bcl_err(nhi) == BCL_ERROR_NONE);
    assert(bcl_cmp(got, nlo) >= 0);
    assert(hi_inclusive ? bcl_cmp(got, nhi) <= 0 : bcl_cmp(got, nhi) < 0);
    bcl_num_free(nlo);
    bcl_num_free(nhi);
}

static void test_external_value_conversion(ScVm *vm)
{
    struct Case {
        const char *input;
        ScExternalValueType type;
        int64_t integer;
        const char *text;
    } cases[] = {
        { "0", SC_EXTERNAL_INTEGER, INT64_C(0), NULL },
        { "9223372036854775807", SC_EXTERNAL_INTEGER, INT64_MAX, NULL },
        { "-9223372036854775808", SC_EXTERNAL_INTEGER, INT64_MIN, NULL },
        { "9223372036854775808", SC_EXTERNAL_TEXT, 0, "9223372036854775808" },
        { "-9223372036854775809", SC_EXTERNAL_TEXT, 0, "-9223372036854775809" },
        { "42.000", SC_EXTERNAL_TEXT, 0, "42.000" },
        { ".125", SC_EXTERNAL_TEXT, 0, ".125" },
        { "-0.50", SC_EXTERNAL_TEXT, 0, "-.50" },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        ScValue value = parse_value(vm, cases[i].input);
        ScExternalValue exported;
        char *text = NULL;

        assert(sc_vm_value_to_string(vm, value, &text) == SC_VM_OK);
        assert(text != NULL);
        assert(strcmp(text, cases[i].text ? cases[i].text : cases[i].input) == 0);
        free(text);

        assert(sc_vm_export_value(vm, value, &exported) == SC_VM_OK);
        assert(exported.type == cases[i].type);
        if (exported.type == SC_EXTERNAL_INTEGER) {
            assert(exported.value.integer == cases[i].integer);
        } else {
            assert(exported.value.text != NULL);
            assert(strcmp(exported.value.text, cases[i].text) == 0);
            free(exported.value.text);
        }
        assert(sc_vm_release_value(vm, value) == SC_VM_OK);
    }

    {
        char *text = (char *) 1;
        ScExternalValue exported;
        assert(sc_vm_value_to_string(vm, SC_INVALID_VALUE, &text)
               == SC_ERR_INVALID_VALUE);
        assert(text == NULL);
        assert(sc_vm_export_value(vm, SC_INVALID_VALUE, &exported)
               == SC_ERR_INVALID_VALUE);
        assert(sc_vm_value_to_string(NULL, 0, &text) == SC_ERR_INTERNAL);
        assert(sc_vm_value_to_string(vm, vm->zero_value, NULL)
               == SC_ERR_INTERNAL);
        assert(sc_vm_export_value(vm, vm->zero_value, NULL)
               == SC_ERR_INTERNAL);
    }
}

static void test_external_value_import(ScVm *vm)
{
    struct IntCase {
        int64_t input;
        const char *expected;
    } ints[] = {
        { INT64_MIN, "-9223372036854775808" },
        { -1, "-1" },
        { 0, "0" },
        { 1, "1" },
        { INT64_MAX, "9223372036854775807" },
    };
    const char *texts[] = {
        "#0",
        "#-1",
        "#.5",
        "#-.5",
        "#42.000",
        "#9223372036854775808",
        "#-9223372036854775809",
        "#123456789012345678901234567890.01234567890123456789",
    };
    const char *invalid[] = {
        "", "#", "123", "#+1", "#-", "#.", "#1.2.3", "#12x", " #1"
    };
    size_t i;

    for (i = 0; i < sizeof(ints) / sizeof(ints[0]); ++i) {
        ScValue value;
        char *text = NULL;
        assert(sc_vm_import_int64(vm, ints[i].input, &value) == SC_VM_OK);
        assert(sc_vm_value_to_string(vm, value, &text) == SC_VM_OK);
        assert(strcmp(text, ints[i].expected) == 0);
        free(text);
        assert(sc_vm_release_value(vm, value) == SC_VM_OK);
    }

    assert(sc_vm_get_numeric_text_prefix(vm) == '#');
    assert(sc_vm_get_numeric_text_prefix(NULL) == '\0');
    assert(sc_vm_set_numeric_text_prefix(NULL, '@') == SC_ERR_INVALID_PREFIX);

    for (i = 0; i < sizeof(texts) / sizeof(texts[0]); ++i) {
        ScValue value;
        char *text = NULL;
        assert(sc_vm_import_numeric_text(vm, texts[i], strlen(texts[i]), &value)
               == SC_VM_OK);
        assert(sc_vm_value_to_string(vm, value, &text) == SC_VM_OK);
        assert(strcmp(text, texts[i] + 1) == 0
               || (strcmp(texts[i], "#-.5") == 0 && strcmp(text, "-.5") == 0));
        free(text);
        assert(sc_vm_release_value(vm, value) == SC_VM_OK);
    }

    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        ScValue value = 0;
        assert(sc_vm_import_numeric_text(vm, invalid[i], strlen(invalid[i]), &value)
               == SC_ERR_TYPE);
        assert(value == SC_INVALID_VALUE);
        assert(sc_vm_reset_after_error(vm) == SC_VM_OK);
    }

    {
        ScValue value;
        char *text = NULL;
        assert(sc_vm_set_numeric_text_prefix(vm, '@') == SC_VM_OK);
        assert(sc_vm_get_numeric_text_prefix(vm) == '@');
        assert(sc_vm_import_numeric_text(vm, "@123.50", 7u, &value) == SC_VM_OK);
        assert(sc_vm_value_to_string(vm, value, &text) == SC_VM_OK);
        assert(strcmp(text, "123.50") == 0);
        free(text);
        assert(sc_vm_release_value(vm, value) == SC_VM_OK);
        assert(sc_vm_import_numeric_text(vm, "#1", 2u, &value) == SC_ERR_TYPE);
        assert(value == SC_INVALID_VALUE);
        assert(sc_vm_reset_after_error(vm) == SC_VM_OK);

        assert(sc_vm_set_numeric_text_prefix(vm, '\0') == SC_ERR_INVALID_PREFIX);
        assert(sc_vm_get_numeric_text_prefix(vm) == '@');
        assert(sc_vm_error(vm)->code == SC_ERR_INVALID_PREFIX);
        assert(sc_vm_reset_after_error(vm) == SC_VM_OK);
        assert(sc_vm_import_numeric_text(vm, "-0.75", 5u, &value) == SC_ERR_TYPE);
        assert(value == SC_INVALID_VALUE);
        assert(sc_vm_reset_after_error(vm) == SC_VM_OK);
        assert(sc_vm_import_numeric_text(vm, "@-0.75", 6u, &value) == SC_VM_OK);
        assert(sc_vm_value_to_string(vm, value, &text) == SC_VM_OK);
        assert(strcmp(text, "-.75") == 0 || strcmp(text, "-0.75") == 0);
        free(text);
        assert(sc_vm_release_value(vm, value) == SC_VM_OK);
        assert(sc_vm_import_numeric_text(vm, "#1", 2u, &value) == SC_ERR_TYPE);
        assert(value == SC_INVALID_VALUE);
        assert(sc_vm_reset_after_error(vm) == SC_VM_OK);
        assert(sc_vm_set_numeric_text_prefix(vm, '#') == SC_VM_OK);
    }

    {
        const char embedded[] = { '#', '1', '\0', '2' };
        ScValue value = 0;
        assert(sc_vm_import_numeric_text(vm, embedded, sizeof(embedded), &value)
               == SC_ERR_TYPE);
        assert(value == SC_INVALID_VALUE);
        assert(sc_vm_reset_after_error(vm) == SC_VM_OK);
    }

    {
        ScValue value = 0;
        assert(sc_vm_import_int64(NULL, 1, &value) == SC_ERR_INTERNAL);
        assert(sc_vm_import_int64(vm, 1, NULL) == SC_ERR_INTERNAL);
        assert(sc_vm_import_numeric_text(NULL, "#1", 2, &value)
               == SC_ERR_INTERNAL);
        assert(sc_vm_import_numeric_text(vm, "#1", 2, NULL)
               == SC_ERR_INTERNAL);
    }

#ifdef SC_TESTING
    {
        ScValue value = 0;
        sc_test_alloc_fail_at(1u);
        assert(sc_vm_import_numeric_text(vm, "#123.5", 6u, &value)
               == SC_ERR_NO_MEM);
        assert(value == SC_INVALID_VALUE);
        sc_test_alloc_reset();
        assert(sc_vm_reset_after_error(vm) == SC_VM_OK);
    }
#endif

    {
        ScValue args[2], result;
        ScExternalValue exported;
        assert(sc_vm_import_int64(vm, 40, &args[0]) == SC_VM_OK);
        assert(sc_vm_import_numeric_text(vm, "#2", 2u, &args[1]) == SC_VM_OK);
        assert(sc_vm_call_name(vm, "add", 3u, args, 2u, &result) == SC_VM_OK);
        assert(sc_vm_export_value(vm, result, &exported) == SC_VM_OK);
        assert(exported.type == SC_EXTERNAL_INTEGER);
        assert(exported.value.integer == 42);
        assert(sc_vm_release_value(vm, result) == SC_VM_OK);
        assert(sc_vm_release_value(vm, args[0]) == SC_VM_OK);
        assert(sc_vm_release_value(vm, args[1]) == SC_VM_OK);
    }
}

static void test_random_builtins(ScVm *vm)
{
    const char *seed[] = { "123456789012345678901234567890" };
    const char *bound[] = { "1000000000000000000000000000000" };
    const char *places[] = { "12" };
    const char *ibound[] = { "37", "7" };
    const char *badneg[] = { "-1" };
    const char *badfrac[] = { "1.5" };
    ScValue seed_state1, seed_state2, a1, a2, b1, b2, out;
    BclNumber n1, n2;
    unsigned i;

    /* Same seed must reproduce the same mixed sequence. */
    seed_state1 = call_values(vm, "rand_seed", seed, 1);
    a1 = call_values(vm, "irand", bound, 1);
    a2 = call_values(vm, "frand", places, 1);
    seed_state2 = call_values(vm, "rand_seed", seed, 1);
    b1 = call_values(vm, "irand", bound, 1);
    b2 = call_values(vm, "frand", places, 1);
    assert(bcl_cmp(sc_vm_value_number(vm, a1), sc_vm_value_number(vm, b1)) == 0);
    assert(bcl_cmp(sc_vm_value_number(vm, a2), sc_vm_value_number(vm, b2)) == 0);
    assert(bcl_cmp(sc_vm_value_number(vm, seed_state1),
                   sc_vm_value_number(vm, seed_state2)) == 0);
    assert(sc_vm_release_value(vm, seed_state1) == SC_VM_OK);
    assert(sc_vm_release_value(vm, seed_state2) == SC_VM_OK);
    assert(sc_vm_release_value(vm, a1) == SC_VM_OK);
    assert(sc_vm_release_value(vm, a2) == SC_VM_OK);
    assert(sc_vm_release_value(vm, b1) == SC_VM_OK);
    assert(sc_vm_release_value(vm, b2) == SC_VM_OK);

    /* Boundary and scale properties over repeated calls. */
    for (i = 0; i < 128; ++i) {
        out = call_values(vm, "irand", bound, 1);
        assert_between(vm, out, "0", bound[0], 1);
        assert(bcl_num_scale(sc_vm_value_number(vm, out)) == 0u);
        assert(sc_vm_release_value(vm, out) == SC_VM_OK);

        out = call_values(vm, "frand", places, 1);
        assert_between(vm, out, "0", "1", 0);
        assert(bcl_num_scale(sc_vm_value_number(vm, out)) == 12u);
        assert(sc_vm_release_value(vm, out) == SC_VM_OK);

        out = call_values(vm, "ifrand", ibound, 2);
        assert_between(vm, out, "0", ibound[0], 0);
        assert(bcl_num_scale(sc_vm_value_number(vm, out)) == 7u);
        assert(sc_vm_release_value(vm, out) == SC_VM_OK);
    }

    { const char *z[] = { "0" }; call_expect(vm, "irand", z, 1, "0"); }
    { const char *o[] = { "1" }; call_expect(vm, "irand", o, 1, "0"); }
    { const char *z[] = { "0" }; call_expect(vm, "frand", z, 1, "0"); }

    /* Seed inspection and OS reseeding return valid numeric seed values. */
    out = call_values(vm, "rand_seed_get", NULL, 0);
    n1 = sc_vm_value_number(vm, out);
    assert(bcl_err(n1) == BCL_ERROR_NONE);
    assert(sc_vm_release_value(vm, out) == SC_VM_OK);
    out = call_values(vm, "rand_reseed", NULL, 0);
    n2 = sc_vm_value_number(vm, out);
    assert(bcl_err(n2) == BCL_ERROR_NONE);
    assert(sc_vm_release_value(vm, out) == SC_VM_OK);

    /* Invalid domains and places conversion errors. */
    { ScValue arg = parse_value(vm, badneg[0]);
      assert(sc_vm_call_name(vm, "irand", 5, &arg, 1, &out) == SC_ERR_TYPE);
      assert(sc_vm_release_value(vm, arg) == SC_VM_OK); }
    { ScValue arg = parse_value(vm, badfrac[0]);
      assert(sc_vm_call_name(vm, "irand", 5, &arg, 1, &out) == SC_ERR_TYPE);
      assert(sc_vm_release_value(vm, arg) == SC_VM_OK); }
    { ScValue arg = parse_value(vm, badneg[0]);
      assert(sc_vm_call_name(vm, "frand", 5, &arg, 1, &out) == SC_ERR_TYPE);
      assert(sc_vm_release_value(vm, arg) == SC_VM_OK); }
    { ScValue arg = parse_value(vm, badfrac[0]);
      assert(sc_vm_call_name(vm, "frand", 5, &arg, 1, &out) == SC_ERR_TYPE);
      assert(sc_vm_release_value(vm, arg) == SC_VM_OK); }

    /* Fixed arity is enforced at runtime too. */
    { ScValue extra = parse_value(vm, "1");
      assert(sc_vm_call_name(vm, "rand_seed_get", 13, &extra, 1, &out) == SC_ERR_ARG_COUNT);
      assert(sc_vm_call_name(vm, "rand_reseed", 11, &extra, 1, &out) == SC_ERR_ARG_COUNT);
      assert(sc_vm_release_value(vm, extra) == SC_VM_OK); }
}

static void test_numeric_prefix_whitelist(void)
{
    ScVm vm_storage;
    ScVm *vm = &vm_storage;
    const char allowed_punct[] = "#@%!? :^~";
    const unsigned char rejected[] = {
        0, 1, 9, 10, 13, ' ', '0', '5', '9', '_', '-', '+', '.', ',',
        '\\', '/', '$', '[', ']', '{', '}', '"', '\'', ';', '`', '&', '*',
        '(', ')', '<', '>', '=', '|', 0x7f, 0x80, 0xff
    };
    ScCompiler c;
    size_t i;
    char ch;

    assert(sc_vm_init(vm, NULL) == SC_VM_OK);

    for (ch = 'A'; ch <= 'Z'; ++ch) assert(sc_numeric_prefix_valid(ch));
    for (ch = 'a'; ch <= 'z'; ++ch) assert(sc_numeric_prefix_valid(ch));
    for (i = 0; i < sizeof(allowed_punct) - 1u; ++i) {
        if (allowed_punct[i] != ' ') assert(sc_numeric_prefix_valid(allowed_punct[i]));
    }
    for (i = 0; i < sizeof(rejected); ++i)
        assert(!sc_numeric_prefix_valid((char) rejected[i]));

    sc_compiler_init(&c, vm, "", 0u);
    assert(sc_compiler_set_constant_prefix(&c, 'x') == SC_COMPILE_OK);
    assert(sc_compiler_get_constant_prefix(&c) == 'x');
    for (i = 0; i < sizeof(rejected); ++i) {
        assert(sc_compiler_set_constant_prefix(&c, (char) rejected[i]) ==
               SC_ERR_COMPILE_INVALID_PREFIX);
        assert(sc_compiler_get_constant_prefix(&c) == 'x');
    }
    sc_compiler_destroy(&c);

    assert(sc_vm_set_numeric_text_prefix(vm, 'x') == SC_VM_OK);
    assert(sc_vm_get_numeric_text_prefix(vm) == 'x');
    for (i = 0; i < sizeof(rejected); ++i) {
        assert(sc_vm_set_numeric_text_prefix(vm, (char) rejected[i]) ==
               SC_ERR_INVALID_PREFIX);
        assert(sc_vm_get_numeric_text_prefix(vm) == 'x');
        assert(sc_vm_reset_after_error(vm) == SC_VM_OK);
    }
    assert(sc_vm_set_numeric_text_prefix(vm, '#') == SC_VM_OK);
    sc_vm_destroy(vm);
}

static void compile_ok_prefix(ScVm *vm, const char *src, char prefix)
{
    ScCompiler c;
    uint32_t n = 0;
    sc_compiler_init(&c, vm, src, strlen(src));
    assert(sc_compiler_set_constant_prefix(&c, prefix) == SC_COMPILE_OK);
    assert(sc_compiler_get_constant_prefix(&c) == prefix);
    assert(sc_compiler_set_constant_prefix(&c, '\0') == SC_ERR_COMPILE_INVALID_PREFIX);
    assert(sc_compiler_get_constant_prefix(&c) == prefix);
    assert(sc_compiler_error(&c)->code == SC_ERR_COMPILE_INVALID_PREFIX);
    memset(&c.error, 0, sizeof(c.error));
    if (sc_compile_source(&c, &n) != SC_COMPILE_OK) {
        fprintf(stderr, "compile: %s at %u:%u\n", c.error.message,
                c.error.line, c.error.column);
        abort();
    }
    assert(n > 0);
    sc_compiler_destroy(&c);
}

static void test_all_builtins(ScVm *vm)
{
    const char *a2[2];
    const char *a3[3];
    a2[0]="7"; a2[1]="3"; call_expect(vm,"add",a2,2,"10");
    call_expect(vm,"sub",a2,2,"4");
    call_expect(vm,"mul",a2,2,"21");
    call_expect(vm,"div",a2,2,"2");
    call_expect(vm,"mod",a2,2,"1");
    a2[0]="2"; a2[1]="10"; call_expect(vm,"pow",a2,2,"1024");
    a2[0]="3"; a2[1]="4"; call_expect(vm,"lshift",a2,2,"30000");
    a2[0]="48"; a2[1]="4"; call_expect(vm,"rshift",a2,2,".0048");
    a2[0]="9"; call_expect(vm,"sqrt",a2,1,"3");
    a2[0]="9"; call_expect(vm,"neg",a2,1,"-9");
    a3[0]="4"; a3[1]="13"; a3[2]="497"; call_expect(vm,"modexp",a3,3,"445");
    a2[0]="2"; a2[1]="3";
    call_expect(vm,"eq",a2,2,"0"); call_expect(vm,"ne",a2,2,"1");
    call_expect(vm,"lt",a2,2,"1"); call_expect(vm,"le",a2,2,"1");
    call_expect(vm,"gt",a2,2,"0"); call_expect(vm,"ge",a2,2,"0");
    a2[0]="3"; a2[1]="3";
    call_expect(vm,"eq",a2,2,"1"); call_expect(vm,"le",a2,2,"1");
    call_expect(vm,"ge",a2,2,"1");
}

static void test_compiled_literals(ScVm *vm)
{
    ScValue out;
    compile_ok_prefix(vm,
        "proc decimal_const {} { return [add #234.456 #0.544] }\n"
        "proc divmod_demo {} { return [add [div #17 #5] [mod #17 #5]] }\n"
        "proc power_demo {} { return [pow #2 #8] }\n", '#');
    assert(sc_vm_call_name(vm,"decimal_const",13,NULL,0,&out)==SC_VM_OK);
    assert_value(vm,out,"235.000"); assert(sc_vm_release_value(vm,out)==SC_VM_OK);
    assert(sc_vm_call_name(vm,"divmod_demo",11,NULL,0,&out)==SC_VM_OK);
    assert_value(vm,out,"5"); assert(sc_vm_release_value(vm,out)==SC_VM_OK);
    assert(sc_vm_call_name(vm,"power_demo",10,NULL,0,&out)==SC_VM_OK);
    assert_value(vm,out,"256"); assert(sc_vm_release_value(vm,out)==SC_VM_OK);

    compile_ok_prefix(vm,
        "proc custom_prefix {} { return [mul @12 @3] }\n", '@');
    assert(sc_vm_call_name(vm,"custom_prefix",13,NULL,0,&out)==SC_VM_OK);
    assert_value(vm,out,"36"); assert(sc_vm_release_value(vm,out)==SC_VM_OK);

    compile_ok_prefix(vm,
        "proc random_compiled {} { rand_seed #42; return [ifrand #10 #4] }\n"
        "proc random_seed_value {} { return [rand_seed_get] }\n", '#');
    assert(sc_vm_call_name(vm,"random_compiled",15,NULL,0,&out)==SC_VM_OK);
    assert_between(vm,out,"0","10",0);
    assert(bcl_num_scale(sc_vm_value_number(vm,out))==4u);
    assert(sc_vm_release_value(vm,out)==SC_VM_OK);
    assert(sc_vm_call_name(vm,"random_seed_value",17,NULL,0,&out)==SC_VM_OK);
    assert(bcl_err(sc_vm_value_number(vm,out))==BCL_ERROR_NONE);
    assert(sc_vm_release_value(vm,out)==SC_VM_OK);
}


static void compile_fail_bytes(const unsigned char *src, size_t len,
                               ScCompileResult expected)
{
    ScVm vm;
    ScCompiler c;
    uint32_t n = 0;
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    sc_compiler_init(&c, &vm, (const char *) src, len);
    assert(sc_compile_source(&c, &n) == expected);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}

static void test_malformed_numeric_literals(void)
{
    static const unsigned char embedded_nul[] =
        "proc f {} {return [mod #17 #517 #\0";
    static const unsigned char bad_char[] =
        "proc f {} {return [add #12x #1]}";
    static const unsigned char prefix_only[] =
        "proc f {} {return #}";
    static const unsigned char two_radices[] =
        "proc f {} {return #1.2.3}";

    compile_fail_bytes(embedded_nul, sizeof(embedded_nul) - 1u,
                       SC_ERR_COMPILE_SYNTAX);
    compile_fail_bytes(bad_char, sizeof(bad_char) - 1u,
                       SC_ERR_COMPILE_INVALID_CONSTANT);
    compile_fail_bytes(prefix_only, sizeof(prefix_only) - 1u,
                       SC_ERR_COMPILE_INVALID_CONSTANT);
    compile_fail_bytes(two_radices, sizeof(two_radices) - 1u,
                       SC_ERR_COMPILE_INVALID_CONSTANT);
}

static uint32_t live_value_count(const ScVm *vm)
{
    uint32_t i, n = 0;
    for (i = 0; i < vm->slot_count; ++i)
        if (vm->slots[i].refcnt != 0) ++n;
    return n;
}

static void test_errors(ScVm *vm)
{
    ScValue args[2], out;
    uint32_t before;
    unsigned i;
    args[0]=parse_value(vm,"1"); args[1]=parse_value(vm,"0");
    before = live_value_count(vm);
    for (i = 0; i < 2000; ++i) {
        assert(sc_vm_call_name(vm,"div",3,args,2,&out)==SC_ERR_DIVZERO);
        assert(sc_vm_call_name(vm,"mod",3,args,2,&out)==SC_ERR_DIVZERO);
        assert(live_value_count(vm) == before);
    }
    assert(sc_vm_release_value(vm,args[0])==SC_VM_OK);
    assert(sc_vm_release_value(vm,args[1])==SC_VM_OK);
    assert(sc_vm_call_name(vm,"missing",7,NULL,0,&out)==SC_ERR_INVALID_SYMBOL);
}

static void test_gc_aliasing(ScVm *vm)
{
    ScValue out;
    uint32_t roots;
    unsigned i;
    compile_ok_prefix(vm,
        "proc alias_stress {x} {\n set y $x\n set y $y\n return [add $x $y]\n}\n"
        "proc nested_stress {x} {\n set y [mul $x #2]\n set z [add $y $x]\n return [sub $z $y]\n}\n", '#');
    roots = live_value_count(vm);
    for (i = 0; i < 3000; ++i) {
        ScValue arg = parse_value(vm, "17");
        assert(sc_vm_call_name(vm, "alias_stress", 12, &arg, 1, &out) == SC_VM_OK);
        assert_value(vm, out, "34");
        assert(sc_vm_release_value(vm, out) == SC_VM_OK);
        assert(sc_vm_call_name(vm, "nested_stress", 13, &arg, 1, &out) == SC_VM_OK);
        assert_value(vm, out, "17");
        assert(sc_vm_release_value(vm, out) == SC_VM_OK);
        assert(sc_vm_release_value(vm, arg) == SC_VM_OK);
        assert(live_value_count(vm) == roots);
    }
}

static void test_vm_once(void)
{
    ScVm vm;
    ScValue out, arg;
    unsigned i;
    assert(sc_vm_init(&vm,NULL)==SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm)==SC_VM_OK);

    assert(sc_vm_call_name(&vm,"get_scale",9,NULL,0,&out)==SC_VM_OK);
    assert_value(&vm,out,"0"); assert(sc_vm_release_value(&vm,out)==SC_VM_OK);
    arg=parse_value(&vm,"7");
    assert(sc_vm_call_name(&vm,"set_scale",9,&arg,1,&out)==SC_VM_OK);
    assert_value(&vm,out,"7"); assert(bcl_ctxt_scale(vm.ctx)==7);
    assert(sc_vm_release_value(&vm,out)==SC_VM_OK);
    assert(sc_vm_release_value(&vm,arg)==SC_VM_OK);
    arg=parse_value(&vm,"0");
    assert(sc_vm_call_name(&vm,"set_scale",9,&arg,1,&out)==SC_VM_OK);
    assert(sc_vm_release_value(&vm,out)==SC_VM_OK);
    assert(sc_vm_release_value(&vm,arg)==SC_VM_OK);

    test_all_builtins(&vm);
    test_random_builtins(&vm);
    test_external_value_conversion(&vm);
    test_external_value_import(&vm);
    test_compiled_literals(&vm);
    test_errors(&vm);
    test_gc_aliasing(&vm);

    /* Repeated calls exercise slot reuse/refcount cleanup. */
    for (i=0;i<5000;i++) {
        const char *a[2]={"123.5","2"};
        call_expect(&vm,"mul",a,2,"247.0");
    }
    sc_vm_destroy(&vm);
}


typedef struct TextBuf {
    char data[4096];
    size_t len;
    unsigned calls;
    unsigned stop_after;
} TextBuf;

static bool collect_disasm(void *ctx, const char *text, size_t len)
{
    TextBuf *b = (TextBuf *)ctx;
    b->calls++;
    if (b->stop_after != 0u && b->calls > b->stop_after) return false;
    assert(b->len + len < sizeof(b->data));
    memcpy(b->data + b->len, text, len);
    b->len += len;
    b->data[b->len] = '\0';
    return true;
}

static ScVmResult builtin_identity(ScVm *vm)
{
    ScValue v;
    assert(sc_vm_argc(vm) == 1u);
    if (sc_vm_arg(vm, 0u, &v) != SC_VM_OK) return SC_ERR_ARG_COUNT;
    if (sc_vm_retain_value(vm, v) != SC_VM_OK) return sc_vm_error(vm)->code;
    return sc_vm_set_result(vm, v);
}

static ScVmResult builtin_no_result(ScVm *vm)
{
    (void)vm;
    return SC_VM_OK;
}

static void test_parser_primitives(void)
{
    ScCompiler c;
    ScSlice s;
    const char src[] = "  alpha\t$beta\n";
    sc_compiler_init(&c, NULL, src, sizeof(src) - 1u);
    assert(sc_peek(&c) == ' ');
    sc_skip_inline_whitespace(&c);
    assert(c.column == 3u);
    assert(sc_parse_bareword(&c, &s) == SC_COMPILE_OK);
    assert(sc_slice_equal_cstr(s, "alpha"));
    sc_skip_inline_whitespace(&c);
    assert(sc_parse_variable(&c, &s) == SC_COMPILE_OK);
    assert(sc_slice_equal_cstr(s, "beta"));
    assert(sc_next(&c) == '\n');
    assert(c.line == 2u && c.column == 1u);
    assert(sc_peek(&c) == EOF);
    assert(!sc_match(&c, 'x'));
    assert(sc_expect(&c, 'x', "need x") == SC_ERR_COMPILE_SYNTAX);
    assert(sc_compiler_error(&c)->source_pos == c.pos);
    sc_compiler_destroy(&c);

    assert(sc_hash_bytes("abc", 3u) == sc_hash_bytes("abc", 3u));
    assert(sc_hash_bytes("abc", 3u) != sc_hash_bytes("abd", 3u));
}

static void compile_expect(const char *src, ScCompileResult expected)
{
    ScVm vm;
    ScCompiler c;
    uint32_t n = 999u;
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    sc_compiler_init(&c, &vm, src, strlen(src));
    { ScCompileResult got = sc_compile_source(&c, &n); if (got != expected) { fprintf(stderr, "compile mismatch: expected %d got %d for: %s\nmessage=%s\n", (int)expected, (int)got, src, c.error.message); abort(); } }
    if (expected != SC_COMPILE_OK) {
        assert(c.error.code == expected);
        assert(c.error.message[0] != '\0');
        assert(c.error.line >= 1u && c.error.column >= 1u);
    }
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}

static void test_compile_error_matrix(void)
{
    compile_expect("notproc f {} {return #1}", SC_ERR_COMPILE_INVALID_PROC_DEF);
    compile_expect("proc f {x x} {return $x}", SC_ERR_COMPILE_DUPLICATE_VAR);
    compile_expect("proc f {} {return $x}", SC_ERR_COMPILE_UNKNOWN_VAR);
    compile_expect("proc f {} {return [missing #1]}", SC_ERR_COMPILE_UNKNOWN_FUNC);
    compile_expect("proc f {} {return [add #1]}", SC_COMPILE_OK);
    compile_expect("proc f {} {break; return #0}", SC_ERR_COMPILE_BREAK_OUTSIDE_LOOP);
    compile_expect("proc f {} {set x #1}", SC_ERR_COMPILE_NO_RETURN_VALUE);
    compile_expect("proc f {} {return}", SC_ERR_COMPILE_INVALID_EXPR);
    compile_expect("proc f {} {return [add #1 #2}", SC_ERR_COMPILE_INVALID_EXPR);
    compile_expect("proc f {} {return #1", SC_ERR_COMPILE_UNEXPECTED_EOF);
    compile_expect("proc f {} {return #1} junk", SC_ERR_COMPILE_INVALID_PROC_DEF);
    compile_expect("proc f {} {return #1}\nproc f {} {return #2}", SC_ERR_COMPILE_DUPLICATE_PROC);
    compile_expect("proc f {} {return #1..2}", SC_ERR_COMPILE_INVALID_CONSTANT);
    compile_expect("proc f {} {return #--1}", SC_ERR_COMPILE_INVALID_CONSTANT);
    compile_expect("proc f {} {if #1 {return #1} {return #0}}", SC_COMPILE_OK);
    compile_expect("# comment\n; ; proc f {} { return #1 };", SC_ERR_COMPILE_INVALID_PROC_DEF);
    compile_expect("proc f {\n x\t y \r\n} {return [add $x $y]}", SC_COMPILE_OK);
    compile_expect("proc f {x", SC_ERR_COMPILE_MISSING_BRACE);
    compile_expect("proc f {{x}} {return #1}", SC_ERR_COMPILE_SYNTAX);
}


static char *make_nested_unary(unsigned depth)
{
    size_t cap = 64u + (size_t)depth * 6u;
    char *z = malloc(cap);
    size_t n = 0;
    unsigned i;
    assert(z != NULL);
    n += (size_t)snprintf(z + n, cap - n, "proc f {} {return ");
    for (i = 0; i < depth; ++i) n += (size_t)snprintf(z + n, cap - n, "[neg ");
    n += (size_t)snprintf(z + n, cap - n, "#1");
    for (i = 0; i < depth; ++i) z[n++] = ']';
    z[n++] = '}';
    z[n] = '\0';
    return z;
}

static char *make_right_nested_add(unsigned depth)
{
    size_t cap = 64u + (size_t)depth * 10u;
    char *z = malloc(cap);
    size_t n = 0;
    unsigned i;
    assert(z != NULL);
    n += (size_t)snprintf(z + n, cap - n, "proc f {} {return ");
    for (i = 0; i < depth; ++i) n += (size_t)snprintf(z + n, cap - n, "[add #1 ");
    n += (size_t)snprintf(z + n, cap - n, "#0");
    for (i = 0; i < depth; ++i) z[n++] = ']';
    z[n++] = '}';
    z[n] = '\0';
    return z;
}

static char *make_nested_if(unsigned depth)
{
    size_t cap = 64u + (size_t)depth * 9u;
    char *z = malloc(cap);
    size_t n = 0;
    unsigned i;
    assert(z != NULL);
    n += (size_t)snprintf(z + n, cap - n, "proc f {} {");
    for (i = 0; i < depth; ++i) n += (size_t)snprintf(z + n, cap - n, "if #1 {");
    n += (size_t)snprintf(z + n, cap - n, "return #7");
    for (i = 0; i < depth; ++i) z[n++] = '}';
    z[n++] = '}';
    z[n] = '\0';
    return z;
}

static char *make_nested_loops(unsigned depth)
{
    size_t cap = 96u + (size_t)depth * 16u;
    char *z = malloc(cap);
    size_t n = 0;
    unsigned i;
    assert(z != NULL);
    n += (size_t)snprintf(z + n, cap - n, "proc f {} {");
    for (i = 0; i < depth; ++i) n += (size_t)snprintf(z + n, cap - n, "loop {");
    n += (size_t)snprintf(z + n, cap - n, "break");
    for (i = 0; i < depth; ++i) {
        z[n++] = '}';
        if (i + 1u < depth) n += (size_t)snprintf(z + n, cap - n, ";break");
    }
    n += (size_t)snprintf(z + n, cap - n, ";return #9}");
    return z;
}


static char *make_many_breaks(unsigned count)
{
    size_t cap = 64u + (size_t)count * 7u;
    char *z = malloc(cap);
    size_t n = 0;
    unsigned i;
    assert(z != NULL);
    n += (size_t)snprintf(z + n, cap - n, "proc f {} {loop {");
    for (i = 0; i < count; ++i) n += (size_t)snprintf(z + n, cap - n, "break;");
    n += (size_t)snprintf(z + n, cap - n, "};return #3}");
    return z;
}

static char *make_mixed_nesting(unsigned depth)
{
    size_t cap = 96u + (size_t)depth * 18u;
    char *z = malloc(cap);
    size_t n = 0;
    unsigned i;
    assert(z != NULL);
    n += (size_t)snprintf(z + n, cap - n, "proc f {} {");
    for (i = 0; i < depth; ++i) {
        if ((i & 1u) == 0u) n += (size_t)snprintf(z + n, cap - n, "if #1 {");
        else n += (size_t)snprintf(z + n, cap - n, "loop {");
    }
    n += (size_t)snprintf(z + n, cap - n, "return [neg #1]");
    for (i = depth; i > 0; --i) {
        if (((i - 1u) & 1u) == 0u) z[n++] = '}';
        else n += (size_t)snprintf(z + n, cap - n, ";break}");
    }
    z[n++] = '}';
    z[n] = '\0';
    return z;
}

static ScCompileResult compile_with_limits(const char *src, uint16_t parse_limit,
                                           uint16_t stack_limit, uint16_t loop_limit,
                                           ScVm *out_vm, ScCompiler *out_c)
{
    uint32_t n = 0;
    assert(sc_vm_init(out_vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(out_vm) == SC_VM_OK);
    out_vm->limits.max_parse_depth = parse_limit;
    out_vm->limits.max_stack = stack_limit;
    out_vm->limits.max_loop_depth = loop_limit;
    sc_compiler_init(out_c, out_vm, src, strlen(src));
    return sc_compile_source(out_c, &n);
}

static void test_deep_nesting_boundaries(void)
{
    ScVm vm;
    ScCompiler c;
    ScValue out;
    ScCompileResult r;
    char *z;
    size_t len;

    z = make_nested_unary(11u);
    r = compile_with_limits(z, 12u, SC_MAX_STACK, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_COMPILE_OK);
    assert(sc_vm_call_name(&vm, "f", 1u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "-1");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_nested_unary(12u);
    r = compile_with_limits(z, 12u, SC_MAX_STACK, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_ERR_COMPILE_MAX_NESTING);
    assert(strstr(c.error.message, "nesting") != NULL);
    assert(c.parse_depth == 0u);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_right_nested_add(7u);
    r = compile_with_limits(z, SC_MAX_PARSE_DEPTH, 8u, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_COMPILE_OK);
    assert(sc_vm_call_name(&vm, "f", 1u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "7");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_right_nested_add(8u);
    r = compile_with_limits(z, SC_MAX_PARSE_DEPTH, 8u, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_ERR_COMPILE_MAX_STACK);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_nested_if(10u);
    r = compile_with_limits(z, 11u, SC_MAX_STACK, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_COMPILE_OK);
    assert(sc_vm_call_name(&vm, "f", 1u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "7");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_nested_if(11u);
    r = compile_with_limits(z, 11u, SC_MAX_STACK, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_ERR_COMPILE_MAX_NESTING);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_nested_loops(SC_MAX_LOOP_DEPTH);
    r = compile_with_limits(z, SC_MAX_PARSE_DEPTH, SC_MAX_STACK, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_COMPILE_OK);
    assert(sc_vm_call_name(&vm, "f", 1u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "9");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_nested_loops(SC_MAX_LOOP_DEPTH + 1u);
    r = compile_with_limits(z, SC_MAX_PARSE_DEPTH, SC_MAX_STACK, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_ERR_COMPILE_MAX_LOOP_DEPTH);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_many_breaks(SC_MAX_BREAKS_PER_LOOP);
    r = compile_with_limits(z, SC_MAX_PARSE_DEPTH, SC_MAX_STACK, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_COMPILE_OK);
    assert(sc_vm_call_name(&vm, "f", 1u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "3");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_many_breaks(SC_MAX_BREAKS_PER_LOOP + 1u);
    r = compile_with_limits(z, SC_MAX_PARSE_DEPTH, SC_MAX_STACK, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_ERR_COMPILE_MAX_BREAKS);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_mixed_nesting(10u);
    r = compile_with_limits(z, 12u, SC_MAX_STACK, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_COMPILE_OK);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);

    z = make_nested_unary(10u);
    len = strlen(z);
    z[len - 1u] = '\0';
    r = compile_with_limits(z, 12u, SC_MAX_STACK, SC_MAX_LOOP_DEPTH, &vm, &c);
    assert(r == SC_ERR_COMPILE_UNEXPECTED_EOF);
    assert(c.parse_depth == 0u);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    free(z);
}

static void test_source_reset_and_compile_one(void)
{
    const char *src = "proc first {} {return #1}\nproc second {} {return #2}\n";
    ScVm vm;
    ScCompiler c;
    ScProcId p0, p1;
    ScValue out;
    size_t split;
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    sc_compiler_init(&c, &vm, src, strlen(src));
    assert(sc_compile_proc(&c, &p0) == SC_COMPILE_OK);
    assert(p0 == 0u);
    split = c.pos;
    sc_compiler_reset_source(&c, src, strlen(src), split);
    assert(sc_compile_proc(&c, &p1) == SC_COMPILE_OK);
    assert(p1 == 1u);
    assert(sc_vm_call_name(&vm, "first", 5u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "1");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    assert(sc_vm_call_name(&vm, "second", 6u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "2");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}

static void test_control_flow_and_calls(void)
{
    ScVm vm;
    ScValue arg, out;
    uint32_t roots;
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    compile_ok_prefix(&vm,
        "proc choose {x} { if $x { return #11 } { return #22 } }\n"
        "proc once {} { loop { break }; return #7 }\n"
        "proc inc {x} { return [add $x #1] }\n"
        "proc twice {x} { return [inc [inc $x]] }\n"
        "proc assign {} { set x #4; set x [mul $x #3]; return $x }\n", '#');
    roots = live_value_count(&vm);
    arg = parse_value(&vm, "0");
    assert(sc_vm_call_name(&vm, "choose", 6u, &arg, 1u, &out) == SC_VM_OK);
    assert_value(&vm, out, "22");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    assert(sc_vm_release_value(&vm, arg) == SC_VM_OK);
    arg = parse_value(&vm, "5");
    assert(sc_vm_call_name(&vm, "choose", 6u, &arg, 1u, &out) == SC_VM_OK);
    assert_value(&vm, out, "11");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    assert(sc_vm_call_name(&vm, "twice", 5u, &arg, 1u, &out) == SC_VM_OK);
    assert_value(&vm, out, "7");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    assert(sc_vm_release_value(&vm, arg) == SC_VM_OK);
    assert(sc_vm_call_name(&vm, "once", 4u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "7");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    assert(sc_vm_call_name(&vm, "assign", 6u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "12");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    assert(live_value_count(&vm) == roots);
    sc_vm_destroy(&vm);
}

static void test_symbols_and_custom_builtins(void)
{
    ScVm vm;
    ScSymbolId id, id2;
    ScValue arg, out;
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_symbol_lookup(&vm.symbols, SC_SLICE_LITERAL("none"), &id) == SC_ERR_INVALID_SYMBOL);
    assert(sc_vm_register_cfunc(&vm, "identity", 1u, builtin_identity, &id) == SC_VM_OK);
    assert(sc_symbol_lookup(&vm.symbols, SC_SLICE_LITERAL("identity"), &id2) == SC_VM_OK && id == id2);
    assert(sc_vm_register_cfunc(&vm, "identity", 1u, builtin_identity, NULL) != SC_VM_OK);
    arg = parse_value(&vm, "123.00");
    assert(sc_vm_call(&vm, id, &arg, 1u, &out) == SC_VM_OK);
    assert_value(&vm, out, "123.00");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    assert(sc_vm_call(&vm, id, NULL, 0u, &out) == SC_ERR_ARG_COUNT);
    assert(sc_vm_release_value(&vm, arg) == SC_VM_OK);
    assert(sc_vm_register_cfunc(&vm, "noret", 0u, builtin_no_result, &id2) == SC_VM_OK);
    assert(sc_vm_call(&vm, id2, NULL, 0u, &out) == SC_ERR_NO_RETURN_VALUE);
    assert(sc_vm_declare_proc(&vm, "empty", 0u, &id2) == SC_VM_OK);
    assert(sc_vm_call(&vm, id2, NULL, 0u, &out) == SC_ERR_EMPTY_PROC);
    assert(sc_vm_call(&vm, UINT32_MAX, NULL, 0u, &out) == SC_ERR_INVALID_SYMBOL);
    sc_vm_destroy(&vm);
}

static void test_value_api_errors(void)
{
    ScVm vm;
    ScValue v, out;
    bool z;
    BclNumber n;
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(!sc_vm_value_valid(&vm, SC_INVALID_VALUE));
    assert(sc_vm_retain_value(&vm, SC_INVALID_VALUE) == SC_ERR_INVALID_VALUE);
    assert(sc_vm_release_value(&vm, SC_INVALID_VALUE) == SC_ERR_INVALID_VALUE);
    assert(sc_vm_value_is_zero(&vm, SC_INVALID_VALUE, &z) == SC_ERR_INVALID_VALUE);
    assert(sc_vm_arg(&vm, 0u, &out) == SC_ERR_ARG_COUNT);
    assert(sc_vm_set_result(&vm, SC_INVALID_VALUE) == SC_ERR_INVALID_VALUE);
    n = bcl_parse("0");
    assert(bcl_err(n) == BCL_ERROR_NONE);
    assert(sc_vm_new_value(&vm, n, &v) == SC_VM_OK);
    assert(sc_vm_value_is_zero(&vm, v, &z) == SC_VM_OK && z);
    assert(sc_vm_retain_value(&vm, v) == SC_VM_OK);
    assert(sc_vm_release_value(&vm, v) == SC_VM_OK);
    assert(sc_vm_release_value(&vm, v) == SC_VM_OK);
    assert(!sc_vm_value_valid(&vm, v));
    assert(sc_vm_release_value(&vm, v) == SC_ERR_INVALID_VALUE);
    sc_vm_clear_error(&vm);
    assert(sc_vm_error(&vm)->code == SC_VM_OK);
    sc_vm_destroy(&vm);
}

static void test_one_armed_if_omits_end_jump(void)
{
    ScVm vm;
    ScCompiler c;
    uint32_t n;
    uint32_t i;
    unsigned end_jumps;

    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);

    {
        const char *src = "proc f {x} {if $x {set y #1}; return #0}";
        sc_compiler_init(&c, &vm, src, strlen(src));
    }
    assert(sc_compile_source(&c, &n) == SC_COMPILE_OK && n == 1u);
    end_jumps = 0u;
    for (i = 0u; i < vm.procs[0].code_count; ++i) {
        if (vm.procs[0].code[i].op == SC_OP_JMP
            && vm.procs[0].code[i].hint == SC_JUMP_IF_END) {
            ++end_jumps;
        }
    }
    assert(end_jumps == 0u);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);

    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    {
        const char *src = "proc f {x} {if $x {set y #1} {set y #2}; return #0}";
        sc_compiler_init(&c, &vm, src, strlen(src));
    }
    assert(sc_compile_source(&c, &n) == SC_COMPILE_OK && n == 1u);
    end_jumps = 0u;
    for (i = 0u; i < vm.procs[0].code_count; ++i) {
        if (vm.procs[0].code[i].op == SC_OP_JMP
            && vm.procs[0].code[i].hint == SC_JUMP_IF_END) {
            ++end_jumps;
        }
    }
    assert(end_jumps == 1u);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}

static void test_verifier_and_disassembler(void)
{
    ScVm vm;
    ScCompiler c;
    TextBuf b;
    ScCompileError err;
    ScProc bad;
    uint32_t n;
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    { const char *src = "proc f {x} {if $x {return [add $x #1]} {return #0}}"; sc_compiler_init(&c, &vm, src, strlen(src)); }
    assert(sc_compile_source(&c, &n) == SC_COMPILE_OK && n == 1u);
    assert(sc_verify_proc(&vm, &vm.procs[0], &err) == SC_COMPILE_OK);
    memset(&b, 0, sizeof(b));
    assert(sc_disassemble_proc(&vm, &vm.procs[0], collect_disasm, &b) == SC_VM_OK);
    assert(strstr(b.data, "LOAD") != NULL);
    assert(strstr(b.data, "CALL add 2") != NULL);
    assert(strstr(b.data, "RET") != NULL);
    memset(&b, 0, sizeof(b));
    b.stop_after = 1u;
    assert(sc_disassemble_proc(&vm, &vm.procs[0], collect_disasm, &b) == SC_ERR_RUNTIME);

    memset(&bad, 0, sizeof(bad));
    bad.argc = 1u;
    bad.slot_count = 0u;
    assert(sc_verify_proc(&vm, &bad, &err) == SC_ERR_COMPILE_INTERNAL);
    bad.argc = 0u;
    bad.slot_count = 0u;
    bad.max_stack = 1u;
    bad.code_count = 1u;
    bad.code = calloc(1u, sizeof(*bad.code));
    assert(bad.code != NULL);
    bad.code[0].op = 255u;
    assert(sc_verify_proc(&vm, &bad, &err) == SC_ERR_COMPILE_INTERNAL);
    assert(strstr(err.message, "instruction 0") != NULL);
    sc_proc_destroy(&bad);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}

static void test_runtime_limits_and_bad_bytecode(void)
{
    ScSecurityLimits lim;
    ScVm vm;
    ScCompiler c;
    ScValue out;
    uint32_t n;
    sc_security_limits_default(&lim);
    lim.max_instructions = 20u;
    assert(sc_vm_init(&vm, &lim) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    { const char *src = "proc forever {} {loop {set x #1}; return #0}"; sc_compiler_init(&c, &vm, src, strlen(src)); }
    assert(sc_compile_source(&c, &n) == SC_COMPILE_OK);
    assert(sc_vm_call_name(&vm, "forever", 7u, NULL, 0u, &out) == SC_ERR_MAX_INSTRUCTIONS);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);

    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    {
        ScProc p;
        memset(&p, 0, sizeof(p));
        p.code_count = p.code_capacity = 1u;
        p.max_stack = 1u;
        p.code = calloc(1u, sizeof(*p.code));
        assert(p.code != NULL);
        p.code[0].op = 255u;
        assert(sc_vm_push_frame(&vm, &p, NULL, 0u) == SC_VM_OK);
        assert(sc_vm_step(&vm) == SC_ERR_INVALID_OPCODE);
        while (vm.frame_count) {
            ScFrame *fr = sc_vm_current_frame(&vm);
            free(fr->values);
            memset(fr, 0, sizeof(*fr));
            vm.frame_count--;
        }
        sc_proc_destroy(&p);
    }
    assert(sc_vm_step(&vm) == SC_ERR_RUNTIME);
    assert(sc_vm_pop_frame(&vm, vm.zero_value) == SC_ERR_INTERNAL);
    assert(sc_vm_push_value(&vm, vm.zero_value) == SC_ERR_INVALID_VALUE);
    assert(sc_vm_pop_value(&vm, &out) == SC_ERR_STACK_UNDERFLOW);
    assert(sc_vm_peek_value(&vm, &out) == SC_ERR_STACK_UNDERFLOW);
    assert(sc_vm_load_local(&vm, 0u, &out) == SC_ERR_INVALID_SLOT);
    assert(sc_vm_store_local(&vm, 0u, vm.zero_value) == SC_ERR_INVALID_SLOT);
    sc_vm_destroy(&vm);
}

static void test_compile_limits(void)
{
    ScSecurityLimits lim;
    ScVm vm;
    ScCompiler c;
    uint32_t n;
    sc_security_limits_default(&lim);
    lim.max_args = 2u;
    assert(sc_vm_init(&vm, &lim) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    { const char *src = "proc f {a b c} {return $a}"; sc_compiler_init(&c, &vm, src, strlen(src)); }
    assert(sc_compile_source(&c, &n) == SC_ERR_COMPILE_MAX_ARGS);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);

    sc_security_limits_default(&lim);
    lim.max_locals = 1u;
    assert(sc_vm_init(&vm, &lim) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    { const char *src = "proc f {} {set a #1; set b #2; return $a}"; sc_compiler_init(&c, &vm, src, strlen(src)); }
    assert(sc_compile_source(&c, &n) == SC_ERR_COMPILE_MAX_LOCALS);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);

    sc_security_limits_default(&lim);
    lim.max_stack = 1u;
    assert(sc_vm_init(&vm, &lim) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    { const char *src = "proc f {} {return [add #1 #2]}"; sc_compiler_init(&c, &vm, src, strlen(src)); }
    assert(sc_compile_source(&c, &n) == SC_ERR_COMPILE_MAX_STACK);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}


static void test_forward_declarations_and_private_procs(void)
{
    ScVm vm;
    ScCompiler c;
    ScValue arg, out;
    ScSymbolId external_id;
    uint32_t n, i;
    const char *src;
    const ScCompileError *err;

    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);

    src =
        "proc forward {x} {return [later $x]}\n"
        "proc later {x} {return [add $x #1]}\n"
        "proc even {x} {if [eq $x #0] {return #1} {return [odd [sub $x #1]]}}\n"
        "proc odd {x} {if [eq $x #0] {return #0} {return [even [sub $x #1]]}}\n"
        "proc private_user {x} {return [_helper $x]}\n"
        "proc _helper {x} {return [mul $x #2]}\n"
        "proc wrong_builtin {} {return [add #1]}\n"
        "proc wrong_proc {} {return [needs_two #1]}\n"
        "proc needs_two {x y} {return [add $x $y]}\n";
    sc_compiler_init(&c, &vm, src, strlen(src));
    assert(sc_compile_source(&c, &n) == SC_COMPILE_OK);
    assert(n == 9u);
    sc_compiler_destroy(&c);

    arg = parse_value(&vm, "4");
    assert(sc_vm_call_name(&vm, "forward", 7u, &arg, 1u, &out) == SC_VM_OK);
    assert_value(&vm, out, "5");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    assert(sc_vm_call_name(&vm, "even", 4u, &arg, 1u, &out) == SC_VM_OK);
    assert_value(&vm, out, "1");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    assert(sc_vm_call_name(&vm, "private_user", 12u, &arg, 1u, &out) == SC_VM_OK);
    assert_value(&vm, out, "8");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);
    assert(sc_vm_release_value(&vm, arg) == SC_VM_OK);

    /* A second source string receives a distinct internal name for _helper. */
    src =
        "proc second_private {} {return [_helper]}\n"
        "proc _helper {} {return #99}\n";
    sc_compiler_init(&c, &vm, src, strlen(src));
    assert(sc_compile_source(&c, &n) == SC_COMPILE_OK);
    assert(n == 2u);
    sc_compiler_destroy(&c);
    assert(sc_vm_call_name(&vm, "second_private", 14u, NULL, 0u, &out)
           == SC_VM_OK);
    assert_value(&vm, out, "99");
    assert(sc_vm_release_value(&vm, out) == SC_VM_OK);

    assert(sc_vm_call_name(&vm, "wrong_builtin", 13u, NULL, 0u, &out)
           == SC_ERR_ARG_COUNT);
    assert(sc_vm_call_name(&vm, "wrong_proc", 10u, NULL, 0u, &out)
           == SC_ERR_ARG_COUNT);

    /* Internal digit-prefixed symbols exist, but name-based runtime calls reject them. */
    {
        unsigned private_count = 0;
        for (i = 0; i < vm.symbols.count; ++i) {
            const ScSymbol *sym = &vm.symbols.symbols[i];
            if (sym->name_len && sym->name[0] >= '0' && sym->name[0] <= '9') {
                private_count++;
                assert(sym->kind == SC_SYM_PROC);
                assert(sc_vm_call_name(&vm, sym->name, sym->name_len,
                                       NULL, 0u, &out) == SC_ERR_INVALID_SYMBOL);
            }
        }
        assert(private_count == 2u);
    }

    /* External placeholders are intentionally exempt from source-end checks. */
    assert(sc_vm_declare_proc(&vm, "external_later", 0u, &external_id)
           == SC_VM_OK);
    src = "proc calls_external {} {return [external_later]}";
    sc_compiler_init(&c, &vm, src, strlen(src));
    assert(sc_compile_source(&c, &n) == SC_COMPILE_OK);
    sc_compiler_destroy(&c);
    assert(vm.symbols.symbols[external_id].decl_origin == SC_DECL_EXTERNAL);
    assert(sc_vm_call_name(&vm, "calls_external", 14u, NULL, 0u, &out)
           == SC_ERR_EMPTY_PROC);

    sc_vm_destroy(&vm);

    /* Unknown public functions become placeholders, then fail at source end. */
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    src = "proc f {} {return [missing #1]}";
    sc_compiler_init(&c, &vm, src, strlen(src));
    assert(sc_compile_source(&c, &n) == SC_ERR_COMPILE_UNKNOWN_FUNC);
    err = sc_compiler_error(&c);
    assert(strcmp(err->message, "undefined function \"missing\"") == 0);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);

    /* A private proc from another source string is not visible here. */
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    src = "proc owner {} {return [_only_here]} proc _only_here {} {return #1}";
    sc_compiler_init(&c, &vm, src, strlen(src));
    assert(sc_compile_source(&c, &n) == SC_COMPILE_OK);
    sc_compiler_destroy(&c);
    src = "proc outsider {} {return [_only_here]}";
    sc_compiler_init(&c, &vm, src, strlen(src));
    assert(sc_compile_source(&c, &n) == SC_ERR_COMPILE_UNKNOWN_FUNC);
    err = sc_compiler_error(&c);
    assert(strcmp(err->message, "undefined function \"_only_here\"") == 0);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);

    /* Digit-prefixed source names are reserved for internal private symbols. */
    compile_expect("proc 1private {} {return #1}", SC_ERR_COMPILE_SYNTAX);
}


static void test_slice_native_apis(void)
{
    ScVm vm;
    ScCompiler c;
    ScValue out;
    uint32_t count = 0;
    char source[] = {
        'p','r','o','c',' ','s','l','i','c','e','d',' ','{','}',
        ' ','{','r','e','t','u','r','n',' ','#','4','2','}'
    };
    char second[] = {
        'p','r','o','c',' ','a','g','a','i','n',' ','{','}',
        ' ','{','r','e','t','u','r','n',' ','#','7','}'
    };

    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);

    sc_compiler_init_slice(&c, &vm, sc_slice_make(source, sizeof(source)));
    assert(c.source.p == source);
    assert(c.source.len == sizeof(source));
    assert(sc_compile_source(&c, &count) == SC_COMPILE_OK);
    assert(count == 1u);
    assert(sc_vm_call_slice(&vm, SC_SLICE_LITERAL("sliced"), NULL, 0u, &out)
           == SC_VM_OK);
    assert_value(&vm, out, "42");
    sc_vm_release_value(&vm, out);

    sc_compiler_reset_source_slice(&c, sc_slice_make(second, sizeof(second)), 0u);
    assert(c.source.p == second);
    assert(c.source.len == sizeof(second));
    assert(sc_compile_source(&c, &count) == SC_COMPILE_OK);
    assert(count == 1u);
    assert(sc_vm_call_slice(&vm, SC_SLICE_LITERAL("again"), NULL, 0u, &out)
           == SC_VM_OK);
    assert_value(&vm, out, "7");
    sc_vm_release_value(&vm, out);

    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}



static void test_runtime_error_recovery_and_poisoning(void)
{
    ScVm vm;
    ScCompiler c;
    ScValue out;
    ScVmResult rc;
    ScSymbolId fatal_sid = SC_INVALID_SYMBOL;
    uint32_t count = 0u;
    const char *src =
        "proc divide_by_zero {} {return [div #1 #0]} "
        "proc healthy {} {return #42} "
        "proc corrupt {} {return #7}";

    assert(!sc_vm_error_is_fatal(SC_ERR_DIVZERO));
    assert(!sc_vm_error_is_fatal(SC_ERR_MAX_INSTRUCTIONS));
    assert(sc_vm_error_is_fatal(SC_ERR_INVALID_OPCODE));
    assert(sc_vm_error_is_fatal(SC_ERR_NO_MEM));
    assert(!sc_compile_error_is_fatal(SC_ERR_COMPILE_SYNTAX));
    assert(sc_compile_error_is_fatal(SC_ERR_COMPILE_NO_MEM));
    assert(sc_compile_error_is_fatal(SC_ERR_COMPILE_INTERNAL));

    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    assert(sc_vm_health(&vm) == SC_VM_HEALTH_READY);
    assert(sc_vm_is_usable(&vm));

    sc_compiler_init(&c, &vm, src, strlen(src));
    assert(sc_compile_source(&c, &count) == SC_COMPILE_OK);
    assert(count == 3u);

    rc = sc_vm_call_name(&vm, "divide_by_zero", 14u, NULL, 0u, &out);
    assert(rc == SC_ERR_DIVZERO);
    assert(vm.frame_count == 0u);
    assert(vm.call_args == NULL);
    assert(vm.call_argc == 0u);
    assert(!vm.call_result_set);
    assert(sc_vm_health(&vm) == SC_VM_HEALTH_RECOVERABLE_ERROR);
    assert(sc_vm_is_usable(&vm));
    assert(sc_vm_call_name(&vm, "healthy", 7u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "42");
    sc_vm_release_value(&vm, out);
    assert(sc_vm_health(&vm) == SC_VM_HEALTH_RECOVERABLE_ERROR);
    assert(sc_vm_reset_after_error(&vm) == SC_VM_OK);
    assert(sc_vm_health(&vm) == SC_VM_HEALTH_READY);
    assert(sc_vm_is_usable(&vm));
    assert(sc_vm_error(&vm)->code == SC_VM_OK);
    assert(sc_vm_call_name(&vm, "healthy", 7u, NULL, 0u, &out) == SC_VM_OK);
    assert_value(&vm, out, "42");
    sc_vm_release_value(&vm, out);

    for (uint32_t i = 0; i < vm.symbols.count; ++i) {
        if (vm.symbols.symbols[i].name_len == 7u
            && memcmp(vm.symbols.symbols[i].name, "corrupt", 7u) == 0) {
            fatal_sid = i;
            break;
        }
    }
    assert(fatal_sid != SC_INVALID_SYMBOL);
    vm.procs[vm.symbols.symbols[fatal_sid].target.proc_id].code[0].op = 0xffu;
    rc = sc_vm_call(&vm, fatal_sid, NULL, 0u, &out);
    assert(rc == SC_ERR_INVALID_OPCODE);
    assert(vm.frame_count == 0u);
    assert(sc_vm_health(&vm) == SC_VM_HEALTH_FATAL_ERROR);
    assert(!sc_vm_is_usable(&vm));
    assert(sc_vm_reset_after_error(&vm) == SC_ERR_INVALID_OPCODE);
    assert(sc_vm_call_name(&vm, "healthy", 7u, NULL, 0u, &out)
           == SC_ERR_INVALID_OPCODE);

    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}


static void test_lifecycle_and_error_diagnostics(void)
{
    ScVm vm;
    ScCompiler c;
    ScValue out;
    ScVmResult rc;
    ScSymbolId sid;
    const ScSymbol *sym;
    TextBuf b = {{0}, 0u, 0u, 0u};
    const char *src = "proc broken {} {return #1}";
    uint32_t count = 0u;

    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(vm.bcl_started);
    assert(vm.bcl_initialized);
    assert(vm.context_created);
    assert(vm.context_pushed);
    assert(vm.symbols_initialized);
    assert(!sc_vm_error_has_disassembly(&vm));

    sc_compiler_init(&c, &vm, src, strlen(src));
    assert(sc_compile_source(&c, &count) == SC_COMPILE_OK);
    assert(count == 1u);
    sid = SC_INVALID_SYMBOL;
    for (uint32_t i = 0; i < vm.symbols.count; ++i) {
        if (vm.symbols.symbols[i].name_len == 6u
            && memcmp(vm.symbols.symbols[i].name, "broken", 6u) == 0) {
            sid = i;
            break;
        }
    }
    assert(sid != SC_INVALID_SYMBOL);
    sym = &vm.symbols.symbols[sid];
    assert(sym->kind == SC_SYM_PROC);
    vm.procs[sym->target.proc_id].code[0].op = 0xffu;

    rc = sc_vm_call_name(&vm, "broken", 6u, NULL, 0u, &out);
    assert(rc == SC_ERR_INVALID_OPCODE);
    assert(strcmp(sc_vm_error(&vm)->message, "invalid opcode") == 0);
    assert(sc_vm_error(&vm)->symbol_id == sid);
    assert(sc_vm_error(&vm)->ip == 0u);
    assert(sc_vm_error_has_disassembly(&vm));
    assert(sc_vm_disassemble_error(&vm, collect_disasm, &b) == SC_VM_OK);
    assert(strstr(b.data, "0: ?") != NULL);

    sc_vm_clear_error(&vm);
    assert(!sc_vm_error_has_disassembly(&vm));
    assert(sc_vm_disassemble_error(&vm, collect_disasm, &b) == SC_ERR_RUNTIME);

    sc_compiler_destroy(&c);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    assert(!vm.bcl_started && !vm.bcl_initialized);
    assert(!vm.context_created && !vm.context_pushed);
    assert(!vm.symbols_initialized);
    sc_vm_destroy(&vm);
}


static void test_error_classification_tables(void)
{
    for (int code = SC_VM_OK; code <= SC_ERR_INTERNAL; ++code) {
        bool expected = !(code == SC_VM_OK
            || code == SC_ERR_ARITHMETIC || code == SC_ERR_DIVZERO
            || code == SC_ERR_TYPE || code == SC_ERR_STACK_OVERFLOW
            || code == SC_ERR_FRAME_OVERFLOW || code == SC_ERR_ARG_COUNT
            || code == SC_ERR_NO_RETURN_VALUE
            || code == SC_ERR_MAX_INSTRUCTIONS
            || code == SC_ERR_INVALID_PREFIX
#if SC_ENABLE_PERSISTENT_STATE
            || code == SC_ERR_STATE_NOT_ACTIVE
            || code == SC_ERR_STATE_ALREADY_ACTIVE
            || code == SC_ERR_STATE_READ_ONLY
            || code == SC_ERR_STATE_RANGE
            || code == SC_ERR_STATE_INVALID_COUNT
#endif
            );
        assert(sc_vm_error_is_fatal((ScVmResult)code) == expected);
    }
    for (int code = SC_COMPILE_OK; code <= SC_ERR_COMPILE_INTERNAL; ++code) {
        bool expected = code == SC_ERR_COMPILE_NO_MEM
                     || code == SC_ERR_COMPILE_INTERNAL;
        assert(sc_compile_error_is_fatal((ScCompileResult)code) == expected);
    }
}

static size_t measure_vm_init_allocations(void)
{
    ScVm vm;
    size_t count;
    sc_test_alloc_reset();
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    count = sc_test_alloc_count();
    sc_vm_destroy(&vm);
    sc_test_alloc_reset();
    return count;
}

static void test_vm_init_allocation_failures(void)
{
    size_t allocations = measure_vm_init_allocations();
    assert(allocations > 0u);
    for (size_t fail_at = 1u; fail_at <= allocations; ++fail_at) {
        ScVm vm;
        ScVmResult rc;
        memset(&vm, 0xa5, sizeof(vm));
        sc_test_alloc_fail_at(fail_at);
        rc = sc_vm_init(&vm, NULL);
        assert(rc == SC_ERR_NO_MEM);
        sc_test_alloc_reset();
        sc_vm_destroy(&vm);
        sc_vm_destroy(&vm);
    }
}

static size_t measure_compile_allocations(const char *source)
{
    ScVm vm;
    ScCompiler c;
    uint32_t count = 0u;
    size_t allocations;
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    sc_compiler_init(&c, &vm, source, strlen(source));
    sc_test_alloc_reset();
    assert(sc_compile_source(&c, &count) == SC_COMPILE_OK);
    allocations = sc_test_alloc_count();
    sc_test_alloc_reset();
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
    return allocations;
}

static void test_compile_allocation_failures(void)
{
    const char *source =
        "proc helper {} {return [add #1 #1]} "
        "proc main {} {return [helper]}";
    size_t allocations = measure_compile_allocations(source);
    assert(allocations > 0u);
    for (size_t fail_at = 1u; fail_at <= allocations; ++fail_at) {
        ScVm vm;
        ScCompiler c;
        ScCompileResult rc;
        uint32_t count = 0u;
        assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
        assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
        sc_compiler_init(&c, &vm, source, strlen(source));
        sc_test_alloc_fail_at(fail_at);
        rc = sc_compile_source(&c, &count);
        assert(rc != SC_COMPILE_OK);
        sc_test_alloc_reset();
        sc_compiler_destroy(&c);
        sc_vm_destroy(&vm);
    }
}

static void test_direct_stack_and_frame_boundaries(void)
{
    ScVm vm;
    ScValue v;
    ScCompiler c;
    uint32_t count = 0u;
    const char *src = "proc f {} {return #1}";
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    sc_compiler_init(&c, &vm, src, strlen(src));
    assert(sc_compile_source(&c, &count) == SC_COMPILE_OK);
    assert(count == 1u);
    v = vm.one_value;
    assert(sc_vm_push_frame(&vm, &vm.procs[0], NULL, 0u) == SC_VM_OK);
    free(vm.frames[0].values);
    vm.frames[0].values = malloc(sizeof(ScValue) * vm.limits.max_stack);
    assert(vm.frames[0].values != NULL);
    vm.frames[0].capacity = vm.limits.max_stack;
    for (uint16_t i = 0; i < vm.limits.max_stack; ++i)
        assert(sc_vm_push_value(&vm, v) == SC_VM_OK);
    assert(sc_vm_push_value(&vm, v) == SC_ERR_STACK_OVERFLOW);
    assert(sc_vm_error_is_fatal(sc_vm_error(&vm)->code) == false);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}




struct MathVector {
    unsigned scale;
    const char *name;
    const char *arg;
    const char *expected;
};

#include "math_vectors.inc"
static void test_bc_math_library(void)
{
    ScVm vm;
    ScCompileError error;
    unsigned current_scale = UINT_MAX;
    size_t i;
    const char *j_cases[][3] = {
        { "0", "1", ".76519" },
        { "1", "1", ".44005" },
        { "2", "3", ".48609" }
    };    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    assert(sc_vm_load_libraries(&vm, SC_LIBRARY_1, &error) == SC_COMPILE_OK);
    assert(sc_vm_loaded_library_level(&vm) == SC_LIBRARY_1);
    for (i = 0u; i < sizeof(bc_math_vectors) / sizeof(bc_math_vectors[0]); ++i) {
        const struct MathVector *v = &bc_math_vectors[i];
        const char *args[] = { v->arg };
        if (current_scale != v->scale) {
            char scale[32];
            const char *scale_args[] = { scale };
            snprintf(scale, sizeof(scale), "%u", v->scale);
            call_expect(&vm, "set_scale", scale_args, 1u, scale);
            current_scale = v->scale;
        }
        call_math_expect(&vm, v->name, args, 1u, v->expected);
    }
    {
        const char *scale[] = { "5" };
        call_expect(&vm, "set_scale", scale, 1u, "5");
    }
    for (i = 0u; i < sizeof(j_cases) / sizeof(j_cases[0]); ++i) {
        const char *args[] = { j_cases[i][0], j_cases[i][1] };
        call_expect(&vm, "j", args, 2u, j_cases[i][2]);
    }
    {
        static const struct {
            const char *scale;
            const char *zero;
            const char *one;
        } identities[] = {
            { "20", "0.00000000000000000000", "1.00000000000000000000" },
            { "65", "0.00000000000000000000000000000000000000000000000000000000000000000",
                    "1.00000000000000000000000000000000000000000000000000000000000000000" }
        };
        size_t k;
        for (k = 0u; k < sizeof(identities) / sizeof(identities[0]); ++k) {
            const char *scale_args[] = { identities[k].scale };
            const char *zero_args[] = { "0" };
            const char *one_args[] = { "1" };
            call_expect(&vm, "set_scale", scale_args, 1u, identities[k].scale);
            call_expect(&vm, "s", zero_args, 1u, identities[k].zero);
            call_expect(&vm, "c", zero_args, 1u, identities[k].one);
            call_expect(&vm, "a", zero_args, 1u, identities[k].zero);
            call_expect(&vm, "e", zero_args, 1u, identities[k].one);
            call_expect(&vm, "l", one_args, 1u, identities[k].zero);
            call_expect(&vm, "get_scale", NULL, 0u, identities[k].scale);
        }
    }
    sc_vm_destroy(&vm);
}


struct Lib2Vector {
    const char *name;
    uint16_t argc;
    const char *arg1;
    const char *arg2;
    const char *expected;
};

#include "lib2_vectors.inc"

static void test_bc_lib2_library(void)
{
    ScVm vm;
    ScCompileError error;
    size_t i;
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    assert(sc_vm_load_libraries(&vm, SC_LIBRARY_1_2, &error) == SC_COMPILE_OK);
    assert(sc_vm_loaded_library_level(&vm) == SC_LIBRARY_1_2);
    { const char *x[] = { "20" }; call_expect(&vm, "set_scale", x, 1u, "20"); }
    for (i = 0u; i < sizeof(bc_lib2_vectors) / sizeof(bc_lib2_vectors[0]); ++i) {
        const struct Lib2Vector *v = &bc_lib2_vectors[i];
        const char *args[] = { v->arg1, v->arg2 };
        call_expect(&vm, v->name, args, v->argc, v->expected);
        call_expect(&vm, "get_scale", NULL, 0u, "20");
    }
    {
        static const struct Lib2Vector bit_vectors[] = {
            { "band", 2, "255", "170", "170" },
            { "bor", 2, "240", "15", "255" },
            { "bxor", 2, "255", "170", "85" },
            { "bnot8", 1, "0", "", "255" },
            { "bnot8", 1, "255", "", "0" },
            { "brev8", 1, "1", "", "128" },
            { "brol8", 2, "129", "1", "3" },
            { "bror8", 2, "3", "1", "129" },
            { "bmod8", 1, "511", "", "255" },
            { "s2un", 2, "-1", "1", "255" }
        };
        for (i = 0u; i < sizeof(bit_vectors) / sizeof(bit_vectors[0]); ++i) {
            const struct Lib2Vector *v = &bit_vectors[i];
            const char *args[] = { v->arg1, v->arg2 };
            call_expect(&vm, v->name, args, v->argc, v->expected);
            call_expect(&vm, "get_scale", NULL, 0u, "20");
        }
    }
    sc_vm_destroy(&vm);
}


struct Lib3Vector {
    const char *name;
    const char *arg;
    const char *places;
    const char *expected;
    unsigned mode;
};

#include "lib3_vectors.inc"

static void test_bc_lib3_rounding_library(void)
{
    ScVm vm;
    ScCompileError error;
    size_t i;
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    assert(sc_vm_load_libraries(&vm, SC_LIBRARY_1_2_3, &error) == SC_COMPILE_OK);
    assert(sc_vm_loaded_library_level(&vm) == SC_LIBRARY_1_2_3);
    { const char *x[] = { "30" }; call_expect(&vm, "set_scale", x, 1u, "30"); }
    for (i = 0u; i < sizeof(bc_lib3_vectors) / sizeof(bc_lib3_vectors[0]); ++i) {
        const struct Lib3Vector *v = &bc_lib3_vectors[i];
        char mode[16];
        const char *args[] = { v->arg, v->places };
        const char *dispatch[] = { v->arg, v->places, mode };
        snprintf(mode, sizeof(mode), "%u", v->mode);
        call_expect_numeric(&vm, v->name, args, 2u, v->expected);
        call_expect_numeric(&vm, "round_mode", dispatch, 3u, v->expected);
        call_expect(&vm, "get_scale", NULL, 0u, "30");
    }
    sc_vm_destroy(&vm);
}


static void test_embedded_library_loader(void)
{
    ScVm vm;
    ScCompileError error;

    assert(sc_vm_embedded_library_level() == SC_LIBRARY_1_2_3);
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    assert(sc_vm_loaded_library_level(&vm) == SC_LIBRARY_NONE);
    assert(sc_vm_load_libraries(&vm, SC_LIBRARY_NONE, &error) == SC_COMPILE_OK);
    assert(sc_vm_loaded_library_level(&vm) == SC_LIBRARY_NONE);
    assert(sc_vm_load_libraries(&vm, SC_LIBRARY_1, &error) == SC_COMPILE_OK);
    assert(sc_vm_loaded_library_level(&vm) == SC_LIBRARY_1);
    assert(sc_vm_load_libraries(&vm, SC_LIBRARY_1, &error) == SC_COMPILE_OK);
    assert(sc_vm_load_libraries(&vm, SC_LIBRARY_NONE, &error) == SC_COMPILE_OK);
    assert(sc_vm_loaded_library_level(&vm) == SC_LIBRARY_1);
    assert(sc_vm_load_libraries(&vm, SC_LIBRARY_1_2_3, &error) == SC_COMPILE_OK);
    assert(sc_vm_loaded_library_level(&vm) == SC_LIBRARY_1_2_3);
    assert(sc_vm_load_libraries(&vm, (ScLibraryLevel) 4, &error) ==
           SC_ERR_COMPILE_INVALID_LIBRARY_LEVEL);
    assert(error.code == SC_ERR_COMPILE_INVALID_LIBRARY_LEVEL);
    assert(sc_vm_load_libraries(NULL, SC_LIBRARY_NONE, &error) ==
           SC_ERR_COMPILE_INVALID_LIBRARY_LEVEL);
    sc_vm_destroy(&vm);
}

static void test_compiler_proc_scratch_cleanup(void)
{
    ScVm vm;
    ScCompiler c;
    ScProcId pid;
    const char ok[] = "proc clean {a} {set x $a;loop {break};return $x}";
    const char bad[] = "proc broken {a} {set x $a;loop {break};set y}";
    uint16_t i;
    uint32_t j;

    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    sc_compiler_init(&c, &vm, ok, strlen(ok));
    assert(sc_compile_proc(&c, &pid) == SC_COMPILE_OK);
    assert(c.current_proc == NULL);
    assert(c.current_proc_symbol == SC_INVALID_SYMBOL);
    assert(!c.in_proc && !c.has_return);
    assert(c.scope.count == 0u && c.break_patch_count == 0u);
    assert(c.loop_depth == 0u && c.parse_depth == 0u);
    assert(c.stack_depth == 0u && c.max_stack == 0u);
    for (i = 0u; i < c.scope.capacity; ++i) {
        assert(c.scope.locals[i].name.p == NULL);
        assert(c.scope.locals[i].name.len == 0u);
    }
    for (j = 0u; j < c.break_patch_capacity; ++j)
        assert(c.break_patches[j].instruction == 0u);

    sc_compiler_reset_source(&c, bad, strlen(bad), 0u);
    assert(sc_compile_proc(&c, NULL) != SC_COMPILE_OK);
    assert(c.current_proc == NULL);
    assert(c.current_proc_symbol == SC_INVALID_SYMBOL);
    assert(!c.in_proc && !c.has_return);
    assert(c.scope.count == 0u && c.break_patch_count == 0u);
    assert(c.loop_depth == 0u && c.parse_depth == 0u);
    assert(c.stack_depth == 0u && c.max_stack == 0u);

    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}


#if SC_ENABLE_PERSISTENT_STATE
static void test_persistent_state(void)
{
    ScVm vm;
    ScCompiler c;
    ScPersistentState state;
    ScValue value = SC_INVALID_VALUE;
    ScValue result = SC_INVALID_VALUE;
    ScSymbolId sid;
    char *text = NULL;
    const char source[] =
        "proc state_inc {x} {state_set #0 [add [state_get #0] $x];return [state_get #0]}\n"
        "proc state_fail {x} {state_set #0 $x;return [div #1 #0]}\n"
        "proc state_read {} {return [state_get #0]}\n"
        "proc state_write {} {return [state_set #0 #99]}";

    memset(&state, 0, sizeof(state));
    assert(sc_vm_init(&vm, NULL) == SC_VM_OK);
    assert(sc_vm_register_core_builtins(&vm) == SC_VM_OK);
    sc_compiler_init(&c, &vm, source, strlen(source));
    { uint32_t n = 0u; assert(sc_compile_source(&c, &n) == SC_COMPILE_OK); assert(n == 4u); }

    assert(sc_persistent_state_init(&vm, &state, 2u) == SC_VM_OK);
    assert(sc_vm_state_count(&vm) == 0u);
    assert(sc_vm_state_get(&vm, 0u, &value) == SC_ERR_STATE_NOT_ACTIVE);
    assert(sc_vm_reset_after_error(&vm) == SC_VM_OK);

    assert(sc_vm_state_begin(&vm, &state, SC_STATE_ACCESS_READ_WRITE) == SC_VM_OK);
    assert(sc_vm_state_active(&vm));
    assert(sc_vm_state_count(&vm) == 2u);
    assert(sc_vm_import_numeric_text(&vm, "#5", 2u, &value) == SC_VM_OK);
    assert(sc_symbol_lookup(&vm.symbols, SC_SLICE_LITERAL("state_inc"), &sid) == SC_VM_OK);
    assert(sc_vm_call(&vm, sid, &value, 1u, &result) == SC_VM_OK);
    assert(sc_vm_release_value(&vm, result) == SC_VM_OK);
    assert(sc_vm_release_value(&vm, value) == SC_VM_OK);
    assert(sc_vm_state_commit(&vm) == SC_VM_OK);

    assert(sc_vm_state_begin(&vm, &state, SC_STATE_ACCESS_READ_ONLY) == SC_VM_OK);
    assert(sc_symbol_lookup(&vm.symbols, SC_SLICE_LITERAL("state_read"), &sid) == SC_VM_OK);
    assert(sc_vm_call(&vm, sid, NULL, 0u, &result) == SC_VM_OK);
    assert(sc_vm_value_to_string(&vm, result, &text) == SC_VM_OK);
    assert(strcmp(text, "5") == 0);
    free(text); text = NULL;
    assert(sc_vm_release_value(&vm, result) == SC_VM_OK);
    assert(sc_symbol_lookup(&vm.symbols, SC_SLICE_LITERAL("state_write"), &sid) == SC_VM_OK);
    assert(sc_vm_call(&vm, sid, NULL, 0u, &result) == SC_ERR_STATE_READ_ONLY);
    assert(!sc_vm_state_active(&vm));
    assert(sc_vm_reset_after_error(&vm) == SC_VM_OK);

    assert(sc_vm_state_begin(&vm, &state, SC_STATE_ACCESS_READ_WRITE) == SC_VM_OK);
    assert(sc_vm_import_numeric_text(&vm, "#42", 3u, &value) == SC_VM_OK);
    assert(sc_symbol_lookup(&vm.symbols, SC_SLICE_LITERAL("state_fail"), &sid) == SC_VM_OK);
    assert(sc_vm_call(&vm, sid, &value, 1u, &result) == SC_ERR_DIVZERO);
    assert(!sc_vm_state_active(&vm));
    assert(sc_vm_release_value(&vm, value) == SC_VM_OK);
    assert(sc_vm_reset_after_error(&vm) == SC_VM_OK);

    assert(sc_vm_state_begin(&vm, &state, SC_STATE_ACCESS_READ_ONLY) == SC_VM_OK);
    assert(sc_vm_state_get(&vm, 0u, &value) == SC_VM_OK);
    assert(sc_vm_value_to_string(&vm, value, &text) == SC_VM_OK);
    assert(strcmp(text, "5") == 0);
    free(text);
    sc_vm_state_rollback(&vm);

    assert(sc_vm_state_begin(&vm, &state, SC_STATE_ACCESS_READ_WRITE) == SC_VM_OK);
    assert(sc_vm_state_begin(&vm, &state, SC_STATE_ACCESS_READ_WRITE) == SC_ERR_STATE_ALREADY_ACTIVE);
    sc_vm_state_rollback(&vm);
    assert(sc_vm_reset_after_error(&vm) == SC_VM_OK);
    assert(sc_persistent_state_init(&vm, &(ScPersistentState){0}, 0u) == SC_ERR_STATE_INVALID_COUNT);
    assert(sc_vm_reset_after_error(&vm) == SC_VM_OK);

    sc_persistent_state_destroy(&vm, &state);
    sc_compiler_destroy(&c);
    sc_vm_destroy(&vm);
}
#endif

int main(void)
{
#if SC_ENABLE_PERSISTENT_STATE
    test_persistent_state();
#endif
    test_bc_lib3_rounding_library();
    test_bc_lib2_library();
    test_bc_math_library();
    test_error_classification_tables();
    test_numeric_prefix_whitelist();
    test_embedded_library_loader();
    test_vm_init_allocation_failures();
    test_compile_allocation_failures();
    test_compiler_proc_scratch_cleanup();
    test_direct_stack_and_frame_boundaries();
    test_parser_primitives();
    test_lifecycle_and_error_diagnostics();
    test_runtime_error_recovery_and_poisoning();
    test_slice_native_apis();
    test_compile_error_matrix();
    test_source_reset_and_compile_one();
    test_deep_nesting_boundaries();
    test_control_flow_and_calls();
    test_forward_declarations_and_private_procs();
    test_symbols_and_custom_builtins();
    test_value_api_errors();
    test_one_armed_if_omits_end_jump();
    test_verifier_and_disassembler();
    test_runtime_limits_and_bad_bytecode();
    test_compile_limits();
    test_malformed_numeric_literals();
    test_vm_once();
    test_vm_once();
    puts("all tests passed");
    return 0;
}
