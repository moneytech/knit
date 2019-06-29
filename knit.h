#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h> //need uintptr_t
#include <stdarg.h>
#include <stdio.h>
//there other includes in the file. 

struct knit_str {
    int ktype;
    char *str; //null terminated
    int len;
    int cap; //negative values mean memory is not owned by us (const char * passed to us)
};

struct knit_list {
    int ktype;
    struct knit_str *items;
    int len;
    int cap;
};

struct knit_obj {
    union knit_obj_u {
        int ktype;
        struct knit_list list;
        struct knit_str str;
    } u;
};
enum KNIT_RV {
    KNIT_OK = 0,
    KNIT_NOMEM,
    KNIT_SYNTAX_ERR,
    KNIT_RUNTIME_ERR,
    KNIT_NOT_FOUND_ERR,
};
enum KNIT_TYPE {
    KNIT_NULL = 0xd6,
    KNIT_STR,
    KNIT_LIST,
};
enum KNIT_OPT {
    KNIT_POLICY_EXIT = 1, //default
    KNIT_POLICY_CONTINUE = 2,
};
enum KNIT_CONF {
    KNIT_MAX_ARGS = 6,
};


static void knit_assert_s(int condition, const char *fmt, ...) {
    (void) fmt;
    assert(condition);
}
static void knit_assert_h(int condition, const char *fmt, ...) {
    (void) fmt;
    assert(condition);
}
static struct knit_str *knit_as_str(struct knit_obj *obj) {
    knit_assert_h(obj->u.ktype == KNIT_STR, "knit_as_str(): invalid argument type, expected string");
    return &obj->u.str;
}
static struct knit_list *knit_as_list(struct knit_obj *obj) {
    knit_assert_h(obj->u.ktype == KNIT_LIST, "knit_as_list(): invalid argument type, expected list");
    return &obj->u.list;
}

#include "hasht/third_party/strhash/superfasthash.h"

/*hashtable defs*/
    typedef struct knit_str    vars_hasht_key_type;   //internal notes: there is no indirection, init/deinit must be used
    typedef struct knit_obj  * vars_hasht_value_type; //                there is indirection,    new/destroy must be used
    int vars_hasht_key_eq_cmp(vars_hasht_key_type *key_1, vars_hasht_key_type *key_2) {
        struct knit_str *str1 = key_1;
        struct knit_str *str2 = key_2;
        if (str1->len != str2->len)
            return 1;
        return memcmp(str1->str, str2->str, str1->len); //0 if eq
    }
    size_t vars_hasht_hash(vars_hasht_key_type *key) {
        struct knit_str *str = key;
        return SuperFastHash(str->str, str->len);
    }

    typedef void * mem_hasht_key_type; 
    typedef unsigned char mem_hasht_value_type; //we don't really need a value, it is a set
    int mem_hasht_key_eq_cmp(mem_hasht_key_type *key_1, mem_hasht_key_type *key_2) {
        //we are comparing two void pointers
        if (*key_1 == *key_2)
            return 0; 
        return 1;
    }
    size_t mem_hasht_hash(mem_hasht_key_type *key) {
        return (uintptr_t) *key; //casting a void ptr to an integer
    }
/*end of hashtable defs*/

#include "vars_hasht.h" //autogenerated and prefixed by vars_
#include "mem_hasht.h"  //autogenerated and prefixed by mem_

struct knit {
    struct vars_hasht vars_ht;
    struct mem_hasht mem_ht;
    char *err_msg;
    unsigned char is_err_msg_owned;
    int err;
    int err_policy;
};


//fwd
static int knit_error(struct knit *knit, int err_type, const char *fmt, ...);

//TODO: make sure no leaks occur during failures of these functions
//      make sure error reporting works in low memory conditions
static int knitx_register_new_ptr_(struct knit *knit, void *m) {
    unsigned char v = 0;
    int rv = mem_hasht_insert(&knit->mem_ht, &m, &v);
    if (rv != MEM_HASHT_OK) {
        return knit_error(knit, KNIT_RUNTIME_ERR, "knitx_register_new_ptr_(): mem_hasht_insert() failed");
    }
    return KNIT_OK;
}
static int knitx_unregister_ptr_(struct knit *knit, void *m) {
    unsigned char v = 0;
    int rv = mem_hasht_remove(&knit->mem_ht, &m);
    if (rv != MEM_HASHT_OK) {
        if (rv == MEM_HASHT_NOT_FOUND) {
            return knit_error(knit, KNIT_RUNTIME_ERR, "knitx_remove_ptr_(): key not found, trying to unregister an unregistered pointer");
        }
        return knit_error(knit, KNIT_RUNTIME_ERR, "knitx_remove_ptr_(): hasht_find() failed");
    }
    return KNIT_OK;
}
static int knitx_update_ptr_(struct knit *knit, void *old_m, void *new_m) {
    if (old_m == new_m)
        return KNIT_OK;
    int rv = KNIT_OK;
    if (old_m != NULL)
        rv = knitx_unregister_ptr_(knit, old_m);
    if (rv != KNIT_OK)
        return rv;
    if (new_m == NULL)
        return KNIT_OK;
    return knitx_register_new_ptr_(knit, new_m);
}

//untracked allocation
static int knitx_rfree(struct knit *knit, void *p) {
    (void) knit;
    free(p);
    return KNIT_OK;
}
static int knitx_rmalloc(struct knit *knit, size_t sz, void **m) {
    knit_assert_h(sz, "knit_malloc(): 0 size passed");
    void *p = malloc(sz);
    *m = NULL;
    if (!p)
        return knit_error(knit, KNIT_NOMEM, "knitx_malloc(): malloc() returned NULL");
    *m = p;
    return KNIT_OK;
}
static int knitx_rstrdup(struct knit *knit, const char *s, char **out_s) {
    (void) knit;
    knit_assert_h(!!s, "passed null string to knitx_rstrdup()");
    size_t len = strlen(s);
    void *p;
    int rv = knitx_rmalloc(knit, len + 1, &p);
    *out_s = p;
    if (rv != KNIT_OK) {
        return rv;
    }
    memcpy(*out_s, s, len);
    (*out_s)[len] = 0;
    return KNIT_OK;
}
static int knitx_rrealloc(struct knit *knit, void *p, size_t sz, void **m) {
    if (!sz) {
        int rv = knitx_rfree(knit, p);
        *m = NULL;
        return rv;
    }
    void *np = realloc(p, sz);
    if (!np) {
        return knit_error(knit, KNIT_NOMEM, "knitx_realloc(): realloc() returned NULL");
    }
    *m = np;
    return KNIT_OK;
}

//tracked allocation
static int knitx_tmalloc(struct knit *knit, size_t sz, void **m) {
    int rv = knitx_rmalloc(knit, sz, m);
    if (rv != KNIT_OK) {
        return rv;
    }
    rv = knitx_register_new_ptr_(knit, *m);
    if (rv != KNIT_OK) {
        knitx_rfree(knit, *m);
        return rv;
    }
    return KNIT_OK;
}
static int knitx_tfree(struct knit *knit, void *p) {
    if (!p)
        return KNIT_OK;
    int rv = knitx_unregister_ptr_(knit, p);
    knitx_rfree(knit, p);
    return rv;
}
static int knitx_trealloc(struct knit *knit, void *p, size_t sz, void **m) {
    int rv = knitx_rrealloc(knit, p, sz, m);
    if (rv != KNIT_OK) {
        return rv;
    }
    rv = knitx_update_ptr_(knit, p, *m);
    return rv;
}




static int knitx_str_init(struct knit *knit, struct knit_str *str) {
    (void) knit;
    str->ktype = KNIT_STR;
    str->str = NULL;
    str->cap = 0;
    str->len = 0;
    return KNIT_OK;
}


static int knitx_str_init_const_str(struct knit *knit, struct knit_str *str, const char *src0) {
    (void) knit;
    knit_assert_h(!!src0, "passed NULL string");
    str->str = (char *) src0;
    str->len = strlen(src0);
    str->cap = -1;
    return KNIT_OK;
}

static int knitx_str_deinit(struct knit *knit, struct knit_str *str) {
    if (str->cap >= 0) {
        knitx_tfree(knit, str->str);
    }
    str->str = NULL;
    str->cap = 0;
    str->len = 0;
    return KNIT_OK;
}

static int knitx_str_new(struct knit *knit, struct knit_str **strp) {
    void *p = NULL;
    int rv = knitx_tmalloc(knit, sizeof(struct knit_str), &p);
    if (rv != KNIT_OK) {
        *strp = NULL;
        return rv;
    }
    rv = knitx_str_init(knit, p);
    *strp = p;
    return rv;
}
static int knitx_str_destroy(struct knit *knit, struct knit_str *strp) {
    int rv = knitx_str_deinit(knit, strp);
    int rv2 = knitx_tfree(knit, strp);
    if (rv != KNIT_OK)
        return rv;
    return rv2;
}

static int knitx_str_clear(struct knit *knit, struct knit_str *str) {
    (void) knit;
    if (str->cap >= 0) {
        str->str[0] = 0;
        str->len = 0;
    }
    else {
        str->str = "";
        str->len = 0;
        str->cap = -1;
    }
    return KNIT_OK;
}
static int knitx_str_set_cap(struct knit *knit, struct knit_str *str, int capacity) {
    if (capacity == 0) {
        return knitx_str_clear(knit, str);
    }
    if (str->cap < 0) {
        void *p;
        int rv  = knitx_tmalloc(knit, capacity, &p);
        if (rv != KNIT_OK) {
            return rv;
        }
        memcpy(p, str->str, str->len);
        str->str = p;
        str->str[str->len] = 0;
    }
    else {
        void *p;
        int rv = knitx_trealloc(knit, str->str, capacity, &p);
        if (rv != KNIT_OK) {
            return rv;
        }
        str->str = p;
    }
    str->cap = capacity;
    return KNIT_OK;
}
static int knitx_str_strlcpy(struct knit *knit, struct knit_str *str, const char *src, int srclen) {
    int rv;
    if (str->cap <= str->len) {
        rv = knitx_str_set_cap(knit, str, srclen + 1);
        if (rv != KNIT_OK) {
            return rv;
        }
    }
    memcpy(str->str, src, srclen);
    str->len = srclen;
    str->str[str->len] = 0;
    return KNIT_OK;
}
static int knitx_str_strlappend(struct knit *knit, struct knit_str *str, const char *src, int srclen) {
    int rv;
    if (str->cap <= (str->len + srclen)) {
        rv = knitx_str_set_cap(knit, str, str->len + srclen + 1);
        if (rv != KNIT_OK) {
            return rv;
        }
    }
    memcpy(str->str + str->len, src, srclen);
    str->len += srclen;
    str->str[str->len] = 0;
    return KNIT_OK;
}
static int knitx_str_strcpy(struct knit *knit, struct knit_str *str, const char *src0) {
    return knitx_str_strlcpy(knit, str, src0, strlen(src0));
}
static int knitx_str_strappend(struct knit *knit, struct knit_str *str, const char *src0) {
    return knitx_str_strlappend(knit, str, src0, strlen(src0));
}

static int knitx_str_init_strcpy(struct knit *knit, struct knit_str *str, const char *src0) {
    int rv = knitx_str_init(knit, str);
    if (rv != KNIT_OK)
        return rv;
    rv = knitx_str_strcpy(knit, str, src0);
    if (rv != KNIT_OK) {
        knitx_str_deinit(knit, str);
        return rv;
    }
    return KNIT_OK;
}

static int knitx_getvar_(struct knit *knit, const char *varname, struct knit_obj **objp) {

    struct knit_str key;
    int rv = knitx_str_init_const_str(knit, &key, varname);
    if (rv != KNIT_OK) {
        return rv;
    }
    struct vars_hasht_iter iter;
    rv = vars_hasht_find(&knit->vars_ht, &key, &iter);
    if (rv != VARS_HASHT_OK) {
        if (rv == VARS_HASHT_NOT_FOUND) 
            return knit_error(knit, KNIT_NOT_FOUND_ERR, "variable '%s' is undefined", varname);
        else 
            return knit_error(knit, KNIT_RUNTIME_ERR, "an error occured while trying to lookup a variable in vars_hasht_find()");
    }
    *objp = iter.pair->value;
    return KNIT_OK;
}
static int knitx_vardump(struct knit *knit, const char *varname) {
    struct knit_obj *valpo;
    int rv = knitx_getvar_(knit, varname, &valpo);
    if (rv == KNIT_OK) {
        if (valpo->u.ktype == KNIT_STR) {
            struct knit_str *valp = &valpo->u.str;
            printf("'%s'", valp->str);
        }
        else if (valpo->u.ktype == KNIT_LIST) {
            struct knit_list *valp = &valpo->u.list;
            printf("LIST");
        }
    }
    else {
        printf("NULL");
    }
    printf("\n");
    return rv;
}

static int knit_vsprintf(struct knit *knit, struct knit_str *str, const char *fmt, va_list ap_in) {
    int fmtlen = strlen(fmt);
    int rv;
    va_list ap;
    if (str->cap < 2) {
        rv = knitx_str_set_cap(knit, str, 2);
        if (rv != KNIT_OK) {
            return rv;
        }
    }
    va_copy(ap, ap_in);
    int needed = vsnprintf(str->str, str->cap, fmt, ap);
    va_end(ap);
    if (needed >= str->cap) {
        rv = knitx_str_set_cap(knit, str, needed + 1);
        if (rv != KNIT_OK) {
            return rv;
        }
        knit_assert_s(needed < str->cap, "str grow failed");
        va_copy(ap, ap_in);
        needed = vsnprintf(str->str, str->cap, fmt, ap);
        knit_assert_s(needed < str->cap, "str grow failed");
        va_end(ap);
    }
    str->len = needed;
    return KNIT_OK;
}
static int knit_sprintf(struct knit *knit, struct knit_str *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rv = knit_vsprintf(knit, str, fmt, ap);
    va_end(ap);
    return rv;
}

static void knit_clear_error(struct knit *knit) {
    knit->err = 0;
    if (knit->is_err_msg_owned)
        knitx_rfree(knit, knit->err_msg);
    knit->err_msg = NULL;
    knit->is_err_msg_owned = 0;
}
static void knit_set_error_policy(struct knit *knit, int policy) {
    knit_assert_h(policy == KNIT_POLICY_CONTINUE || policy == KNIT_POLICY_EXIT, "invalid error policy");
    knit->err_policy = policy;
}
static void knit_error_act(struct knit *knit, int err_type) {
    (void) err_type;
    if (knit->err_policy == KNIT_POLICY_EXIT) {
        if (knit->err_msg)
            fprintf(stderr, "%s\n", knit->err_msg);
        else
            fprintf(stderr, "an unknown error occured (no err msg)\n");
        abort();
        exit(1);
    }
}
static int knit_error(struct knit *knit, int err_type, const char *fmt, ...) {
    knit_assert_h(err_type, "invalid error code passed (0)");
    if (knit->err)
        return knit->err;
    knit->err = err_type;
    va_list ap;
    struct knit_str tmp;
    int rv = knitx_str_init(knit, &tmp);
    if (rv != KNIT_OK) {
        return rv;
    }
    va_start(ap, fmt);
    rv = knit_vsprintf(knit, &tmp, fmt, ap);
    va_end(ap);
    if (rv == KNIT_OK) {
        knit_assert_h(!!tmp.str, "");
        rv = knitx_rstrdup(knit, tmp.str, &knit->err_msg);
    }
    knitx_str_deinit(knit, &tmp);
    knit_error_act(knit, err_type);
    return err_type;
}

static int knitx_init(struct knit *knit, int opts) {
    int rv;
    if (opts & KNIT_POLICY_CONTINUE)
        knit_set_error_policy(knit, KNIT_POLICY_CONTINUE);
    else 
        knit_set_error_policy(knit, KNIT_POLICY_EXIT);

    knit->err_msg = NULL;
    knit->err = KNIT_OK;

    rv = mem_hasht_init(&knit->mem_ht, 256);

    if (rv != MEM_HASHT_OK) {
        rv = knit_error(knit, KNIT_RUNTIME_ERR, "couldn't initialize mem hashtable");
        goto end;
    }
    rv = vars_hasht_init(&knit->vars_ht, 32);
    if (rv != VARS_HASHT_OK) {
        rv = knit_error(knit, KNIT_RUNTIME_ERR, "couldn't initialize vars hashtable");
        goto cleanup_mem_ht;
    }

    return KNIT_OK;

cleanup_vars_ht:
    vars_hasht_deinit(&knit->vars_ht);
cleanup_mem_ht:
    mem_hasht_deinit(&knit->mem_ht);
end:
    return rv;
}
static int knitx_deinit(struct knit *knit) {
    struct mem_hasht_iter iter;
    int rv = mem_hasht_begin_iterator(&knit->mem_ht, &iter);
    knit_assert_h(rv == MEM_HASHT_OK, "knitx_deinit(): couldnt iterate memory set");
    for (; mem_hasht_iter_check(&iter);  mem_hasht_iter_next(&knit->mem_ht, &iter)) {
        void *p = iter.pair->key;
        knitx_rfree(knit, p);
    }
    mem_hasht_deinit(&knit->mem_ht);
    vars_hasht_deinit(&knit->vars_ht);
    return KNIT_OK;
}
static int knitx_set_str(struct knit *knit, const char *key, const char *value) {
    struct knit_str key_str;
    int rv = knitx_str_init(knit, &key_str);
    if (rv != KNIT_OK) {
        return rv;
    }
    rv = knitx_str_strcpy(knit, &key_str, key);
    if (rv != KNIT_OK)
        goto cleanup_key;
    struct knit_str *val_strp;
    rv = knitx_str_new(knit, &val_strp);
    if (rv != KNIT_OK) 
        goto cleanup_key;
    rv = knitx_str_strcpy(knit, val_strp, value);
    if (rv != KNIT_OK) 
        goto cleanup_val;
    //ownership of key is transferred to the vars hashtable
    struct knit_obj *objp = (struct knit_obj *) val_strp; //defined operation?
    rv = vars_hasht_insert(&knit->vars_ht, &key_str, &objp);
    if (rv != VARS_HASHT_OK) {
        rv = knit_error(knit, KNIT_RUNTIME_ERR, "knitx_set_str(): inserting key into vars hashtable failed");
        goto cleanup_val;
    }
    return KNIT_OK;

cleanup_val:
    knitx_str_destroy(knit, val_strp);
cleanup_key:
    knitx_str_deinit(knit, &key_str);
    return rv;
}
static int knitx_get_str(struct knit *knit, const char *varname, struct knit_str **kstrp) {
    struct knit_obj *objp;
    *kstrp = NULL;
    int rv = knitx_getvar_(knit, varname, &objp);
    if (rv != KNIT_OK)
        return rv;
    *kstrp = knit_as_str(objp);
    return KNIT_OK;
}
struct ka_lexer; //fwd
static int knitx_lexdump(struct knit *knit, struct ka_lexer *lexer) {
    return KNIT_OK;
}
static int knitx_lexer_init_str(struct knit *knit, struct ka_lexer *lex, const char *program) {
    return KNIT_OK;
}
static int knitx_lexer_deinit(struct knit *knit, struct ka_lexer *lex) {
    return KNIT_OK;
}

enum KATOK {
    KAT_STRLITERAL,
    KAT_INTLITERAL,
    KAT_VAR,
    KAT_DOT,
    KAT_EQUALS,
    KAT_OPAREN,
    KAT_CPAREN,
    KAT_OBRACKET,
    KAT_CBRACKET,
    KAT_COMMA,
    KAT_COLON,
    KAT_SEMICOLON,
};
enum KAEXP {
    KAX_CALL,
    KAX_BINARY,
    KAX_UNARY,
    KAX_LITERAL_STR,
    KAX_LITERAL_INT,
    KAX_LITERAL_LIST,
    KAX_LIST_INDEX,
    KAX_LIST_SLICE,
    KAX_VAR_REF,
    KAX_OBJ_DOT,
};
enum KASTMT {
    KAS_CALL,
    KAS_ASSIGN,
    KAS_EMPTY,
};
struct katok {
    int toktype;
    int integer;
    struct knit_str str;
};
struct kaexpr {
    int exptype;
    union {

        struct knit_str *str;

        struct knit_list *list;

        int integer;

        struct { 
            int toktype; //op
            struct knit_obj *lhs;
            struct knit_obj *rhs;
        } binary;

        struct { 
            int toktype; //op
            struct knit_obj *side;
        } unary;

        struct kaexpr *exp;

        struct { 
            struct kaexpr *beg;
            struct kaexpr *end;
        } slice;

        struct kadot { 
            struct knit_str *str;
            struct kadot *next;
        };
        struct kaprefix { 
            struct kaexpr *start;
            struct kadot *rest;
        } prefix;

        // foo  . bar   .baz    
        //              _kadot_
        //     _kadot__________
        //_prefix______________
        //
        struct kacall *call;

        /*
        KAX_CALL: call
        KAX_BINARY: binary
        KAX_UNARY: unary
        KAX_LITERAL_STR: str
        KAX_LITERAL_INT: integer
        KAX_LITERAL_LIST: list
        KAX_VAR_REF: str
        KAX_LIST_INDEX: exp
        KAX_LIST_SLICE: slice
        KAX_OBJ_DOT: prefix
        */

    } u;
};
struct kafunc_args {
    struct kaexpr args[KNIT_MAX_ARGS];
    int nargs;
};
typedef struct knit_obj * ( *kafunc_type )(struct knit *, struct kafunc_args *);
struct kafunc {
    kafunc_type fptr;
};
struct kacall {
    struct kaexpr *called;
    struct kafunc_args args;
};
struct knitloc {
    struct knit_str *fname;
    int lineno;
    int colno;
};
struct kastmt {
    int stmttype;
    struct knitloc loc;
    union {
        struct kacall call;
        struct { 
            struct kaexpr *lvalue;
            struct kaexpr *exp;
        } assign;
    } u;
};
struct kachunk { 
    struct kastmt *stmts;
    int stmt_len;
    int stmt_cap;
};

struct ka_exec_state {
    int ip;
    struct kachunk *chunk;
};

struct ka_lexer {
    struct knit_str *filename;
    int lineno;
    int colno;
    struct katok cur_token;
    struct katok la_token;
};


/*
    this is taken from LUA's syntax and modified
    {A} means 0 or more As, and [A] means an optional A.

	program ::= block

	block ::= {stat} 

	stat ::=  ‘;’ | 
		 varlist ‘=’ explist | 
		 functioncall 

	prefixexp ::= var | functioncall | ‘(’ exp ‘)’

	exp ::=  nil | false | true | Numeral | LiteralString | LiteralList | prefixexp |  exp binop exp | unop exp 

	varlist ::= var {‘,’ var}

	var ::=  Name | prefixexp ‘[’ exp ‘]’ | prefixexp ‘.’ Name 

	namelist ::= Name {‘,’ Name}

	explist ::= exp {‘,’ exp}

	functioncall ::=  prefixexp args 

	args ::=  ‘(’ [explist] ‘)’ | tableconstructor | LiteralString 

	binop ::=  ‘+’ | ‘-’ | ‘*’ | ‘/’ | ‘//’ | ‘^’ | ‘%’ | 
		 ‘&’ | ‘~’ | ‘|’ | ‘>>’ | ‘<<’ | ‘..’ | 
		 ‘<’ | ‘<=’ | ‘>’ | ‘>=’ | ‘==’ | ‘!=’ | 
		 and | or | not

	unop ::= ‘-’ | not | ‘#’ | ‘~’

 * */


static int knitx_lexer_init_str(struct knit *knit, struct ka_lexer *lex, const char *program);
static int knitx_lexer_deinit(struct knit *knit, struct ka_lexer *lex);

static int knitx_eval(struct knit *knit, const char *program) {
    struct ka_lexer lexer;
    knitx_lexer_init_str(knit, &lexer, program);
    knitx_lexdump(knit, &lexer);
    knitx_lexer_deinit(knit, &lexer);
    return KNIT_OK; //dummy
}
