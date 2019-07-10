
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

#include "vars_hasht.h" //autogenerated hasht.h and prefixed by vars_
#include "mem_hasht.h"  //autogenerated hasht.h and prefixed by mem_


struct ka_exec_state {
    int ip;
    struct kablock *block;
};

struct katok {
    int toktype;
    int lineno;
    int colno;
    int offset;
    int len;
    union {
        int integer;
    } data;
};
#include "tok_darr.h"  //autogenerated darr.h and prefixed by tok_


struct knit {
    struct vars_hasht vars_ht;
    struct mem_hasht mem_ht;

    struct ka_exec_state ex;

    char *err_msg;
    unsigned char is_err_msg_owned;
    int err;
    int err_policy;
};

enum KATOK {
    KAT_EOF,
    KAT_BOF, //begining of file
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
    KAT_ADD,
    KAT_SUB,
    KAT_MUL,
    KAT_DIV,

    //not tokens
    KAT_ASSOC_LEFT,
    KAT_ASSOC_RIGHT,
};
enum KAEXPR {
    KAX_CALL,
    KAX_LITERAL_STR,
    KAX_LITERAL_INT,
    KAX_LITERAL_LIST,
    KAX_LIST_INDEX,
    KAX_LIST_SLICE,
    KAX_VAR_REF,
    KAX_OBJ_DOT,
};
struct kaexpr {
    int exptype;
    union {

        struct knit_str *str;

        struct knit_list *list;

        int integer;


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
//order tied to knit_insninfo
enum KNIT_INSN {
    KPUSH = 1,
    KPOP,
    KCALL,
    KINDX,
    KADD,
    KSUB,
    KMUL,
    KDIVM,
};
#define KINSN_FIRST KPUSH
#define KINSN_LAST  KDIVM
#define KINSN_TVALID(type)  ((type) >= KINSN_FIRST && (type) <= KINSN_LAST)

//order tied to enum
static struct knit_insninfo {
    int insn_type;
    const char *rep;
} knit_insninfo[] = {
    {0, NULL},
    {KPUSH, "KPUSH"},
    {KPOP, "KPOP"},
    {KCALL, "KCALL"},
    {KINDX, "KINDX"},
    {KADD, "KADD"},
    {KSUB, "KSUB"},
    {KMUL, "KMUL"},
    {KDIVM, "KDIVM"},
};
struct knit_insn {
    int insn_type;
    int op1;
    int op2;
};
#include "insns_darr.h"
struct kablock { 
    struct insns_darr insns;
};
struct knit_lex {
    struct knit_str *filename;
    struct knit_str *input;
    int lineno;
    int colno;
    int offset;
    int tokno; //refers to an index in .tokens

    struct tok_darr tokens;
};
struct knit_prs {
    struct knit_lex lex; //fwd
    struct kaexpr expr;
    struct kablock block;
};