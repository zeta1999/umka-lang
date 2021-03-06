#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "umka_vm.h"
#include "umka_types.h"
#include "umka_ident.h"


static char *spelling [] =
{
    "none",
    "forward",
    "void",
    "null",
    "int8",
    "int16",
    "int32",
    "int",
    "uint8",
    "uint16",
    "uint32",
    "uint",
    "bool",
    "char",
    "real32",
    "real",
    "^",
    "[...]",
    "[]",
    "str",
    "struct",
    "interface",
    "fiber",
    "fn"
};


void typeInit(Types *types, Error *error)
{
    types->first = types->last = NULL;
    types->error = error;
}


void typeFreeFieldsAndParams(Type *type)
{
    if (type->kind == TYPE_STRUCT || type->kind == TYPE_INTERFACE)
        for (int i = 0; i < type->numItems; i++)
            free(type->field[i]);

    else if (type->kind == TYPE_FN)
        for (int i = 0; i < type->sig.numParams; i++)
            free(type->sig.param[i]);
}


void typeFree(Types *types, int startBlock)
{
    Type *type = types->first;

    // If block is specified, fast forward to the first type in this block (assuming this is the last block in the list)
    if (startBlock >= 0)
    {
        while (type && type->next && type->next->block != startBlock)
            type = type->next;

        Type *next = type->next;
        types->last = type;
        types->last->next = NULL;
        type = next;
    }

    while (type)
    {
        Type *next = type->next;
        typeFreeFieldsAndParams(type);
        free(type);
        type = next;
    }
}


Type *typeAdd(Types *types, Blocks *blocks, TypeKind kind)
{
    Type *type = malloc(sizeof(Type));

    type->kind          = kind;
    type->block         = blocks->item[blocks->top].block;
    type->base          = NULL;
    type->numItems      = 0;
    type->weak          = false;
    type->typeIdent     = NULL;
    type->next          = NULL;

    if (kind == TYPE_FN)
    {
        type->sig.method            = false;
        type->sig.offsetFromSelf    = 0;
        type->sig.numParams         = 0;
        type->sig.numDefaultParams  = 0;
        type->sig.numResults        = 0;
    }

    // Add to list
    if (!types->first)
        types->first = types->last = type;
    else
    {
        types->last->next = type;
        types->last = type;
    }
    return types->last;
}


void typeDeepCopy(Type *dest, Type *src)
{
    typeFreeFieldsAndParams(dest);

    Type *next = dest->next;
    *dest = *src;
    dest->next = next;

    if (dest->kind == TYPE_STRUCT || dest->kind == TYPE_INTERFACE)
        for (int i = 0; i < dest->numItems; i++)
        {
            dest->field[i] = malloc(sizeof(Field));
            *(dest->field[i]) = *(src->field[i]);
        }

    else if (dest->kind == TYPE_FN)
        for (int i = 0; i < dest->sig.numParams; i++)
        {
            dest->sig.param[i] = malloc(sizeof(Param));
            *(dest->sig.param[i]) = *(src->sig.param[i]);
        }
}


Type *typeAddPtrTo(Types *types, Blocks *blocks, Type *type)
{
    typeAdd(types, blocks, TYPE_PTR);
    types->last->base = type;
    return types->last;
}


int typeSizeNoCheck(Type *type)
{
    switch (type->kind)
    {
        case TYPE_VOID:     return 0;
        case TYPE_INT8:     return sizeof(int8_t);
        case TYPE_INT16:    return sizeof(int16_t);
        case TYPE_INT32:    return sizeof(int32_t);
        case TYPE_INT:      return sizeof(int64_t);
        case TYPE_UINT8:    return sizeof(uint8_t);
        case TYPE_UINT16:   return sizeof(uint16_t);
        case TYPE_UINT32:   return sizeof(uint32_t);
        case TYPE_UINT:     return sizeof(uint64_t);
        case TYPE_BOOL:     return sizeof(bool);
        case TYPE_CHAR:     return sizeof(char);
        case TYPE_REAL32:   return sizeof(float);
        case TYPE_REAL:     return sizeof(double);
        case TYPE_PTR:
        case TYPE_STR:      return sizeof(void *);
        case TYPE_ARRAY:    return type->numItems * typeSizeNoCheck(type->base);
        case TYPE_DYNARRAY: return sizeof(DynArray);
        case TYPE_STRUCT:
        case TYPE_INTERFACE:
        {
            int size = 0;
            for (int i = 0; i < type->numItems; i++)
                size += typeSizeNoCheck(type->field[i]->type);
            return size;
        }
        case TYPE_FIBER:    return sizeof(Fiber);
        case TYPE_FN:       return sizeof(int64_t);
        default:            return -1;
    }
}


int typeSize(Types *types, Type *type)
{
    int size = typeSizeNoCheck(type);
    if (size < 0)
    {
        char buf[DEFAULT_STR_LEN + 1];
        types->error->handler(types->error->context, "Illegal type %s", typeSpelling(type, buf));
    }
    return size;
}


bool typeGarbageCollected(Type *type)
{
    if (type->kind == TYPE_PTR || type->kind == TYPE_STR || type->kind == TYPE_DYNARRAY || type->kind == TYPE_INTERFACE || type->kind == TYPE_FIBER)
        return true;

    if (type->kind == TYPE_ARRAY)
        return typeGarbageCollected(type->base);

    if (type->kind == TYPE_STRUCT)
        for (int i = 0; i < type->numItems; i++)
            if (typeGarbageCollected(type->field[i]->type))
                return true;

    return false;
}


bool typeEquivalent(Type *left, Type *right)
{
    if (left == right)
        return true;

    if (left->kind == right->kind)
    {
        // Pointers
        if (left->kind == TYPE_PTR)
            return typeEquivalent(left->base, right->base);

        // Arrays
        else if (left->kind == TYPE_ARRAY)
        {
            // Number of elements
            if (left->numItems != right->numItems)
                return false;

            return typeEquivalent(left->base, right->base);
        }

        // Dynamic arrays
        else if (left->kind == TYPE_DYNARRAY)
            return typeEquivalent(left->base, right->base);

        // Strings
        else if (left->kind == TYPE_STR)
            return true;

        // Structures or interfaces
        else if (left->kind == TYPE_STRUCT || left->kind == TYPE_INTERFACE)
        {
            // Number of fields
            if (left->numItems != right->numItems)
                return false;

            // Fields
            for (int i = 0; i < left->numItems; i++)
            {
                // Name
                if (left->field[i]->hash != right->field[i]->hash || strcmp(left->field[i]->name, right->field[i]->name) != 0)
                    return false;

                // Type
                if (!typeEquivalent(left->field[i]->type, right->field[i]->type))
                    return false;
            }
            return true;
        }

        // Functions
        else if (left->kind == TYPE_FN)
        {
            // Number of parameters
            if (left->sig.numParams != right->sig.numParams)
                return false;

            // Method flag
            if (left->sig.method != right->sig.method)
                return false;

            // Parameters (skip interface method receiver)
            int iStart = left->sig.offsetFromSelf == 0 ? 0 : 1;
            for (int i = iStart; i < left->sig.numParams; i++)
            {
                // Name
                if (left->sig.param[i]->hash != right->sig.param[i]->hash ||
                    strcmp(left->sig.param[i]->name, right->sig.param[i]->name) != 0)
                    return false;

                // Type
                if (!typeEquivalent(left->sig.param[i]->type, right->sig.param[i]->type))
                    return false;

                // Default value
                if (left->sig.param[i]->defaultVal.intVal != right->sig.param[i]->defaultVal.intVal)
                    return false;
            }

            // Number of results
            if (left->sig.numResults != right->sig.numResults)
                return false;

            // Result types
            for (int i = 0; i < left->sig.numResults; i++)
                if (!typeEquivalent(left->sig.resultType[i], right->sig.resultType[i]))
                    return false;

            return true;
        }

        // Primitive types
        else
            return true;
    }
    return false;
}


bool typeAssertEquivalent(Types *types, Type *left, Type *right)
{
    bool res = typeEquivalent(left, right);
    if (!res)
    {
        char leftBuf[DEFAULT_STR_LEN + 1], rightBuf[DEFAULT_STR_LEN + 1];
        types->error->handler(types->error->context, "Incompatible types %s and %s", typeSpelling(left, leftBuf), typeSpelling(right, rightBuf));

    }
    return res;
}


bool typeCompatible(Type *left, Type *right, bool symmetric)
{
    if (typeEquivalent(left, right))
        return true;

    // Integers
    if (typeInteger(left) && typeInteger(right))
        return true;

    // Reals
    if (typeReal(left) && typeReal(right))
        return true;

    // Pointers
    if (left->kind == TYPE_PTR && right->kind == TYPE_PTR)
    {
        // Any pointer can be assigned to an untyped pointer
        if (left->base->kind == TYPE_VOID)
            return true;

        // Any pointer can be compared to an untyped pointer
        if (right->base->kind == TYPE_VOID && symmetric)
            return true;

        // Null can be assigned to any pointer
        if (right->base->kind == TYPE_NULL)
            return true;

        // Null can be compared to any pointer
        if (left->base->kind == TYPE_NULL && symmetric)
            return true;
    }
    return false;
}


bool typeAssertCompatible(Types *types, Type *left, Type *right, bool symmetric)
{
    bool res = typeCompatible(left, right, symmetric);
    if (!res)
    {
        char leftBuf[DEFAULT_STR_LEN + 1], rightBuf[DEFAULT_STR_LEN + 1];
        types->error->handler(types->error->context, "Incompatible types %s and %s", typeSpelling(left, leftBuf), typeSpelling(right, rightBuf));
    }
    return res;
}


bool typeValidOperator(Type *type, TokenKind op)
{
    switch (op)
    {
        case TOK_PLUS:      return typeInteger(type) || typeReal(type) || type->kind == TYPE_STR;
        case TOK_MINUS:
        case TOK_MUL:
        case TOK_DIV:       return typeInteger(type) || typeReal(type);
        case TOK_MOD:
        case TOK_AND:
        case TOK_OR:
        case TOK_XOR:
        case TOK_SHL:
        case TOK_SHR:       return typeInteger(type);
        case TOK_PLUSEQ:
        case TOK_MINUSEQ:
        case TOK_MULEQ:
        case TOK_DIVEQ:     return typeInteger(type) || typeReal(type);
        case TOK_MODEQ:
        case TOK_ANDEQ:
        case TOK_OREQ:
        case TOK_XOREQ:
        case TOK_SHLEQ:
        case TOK_SHREQ:     return typeInteger(type);
        case TOK_ANDAND:
        case TOK_OROR:      return type->kind == TYPE_BOOL;
        case TOK_PLUSPLUS:
        case TOK_MINUSMINUS:return typeInteger(type);
        case TOK_EQEQ:      return typeOrdinal(type) || typeReal(type) || type->kind == TYPE_PTR || type->kind == TYPE_STR;
        case TOK_LESS:
        case TOK_GREATER:   return typeOrdinal(type) || typeReal(type) || type->kind == TYPE_STR;
        case TOK_EQ:        return true;
        case TOK_NOT:       return type->kind == TYPE_BOOL;
        case TOK_NOTEQ:     return typeOrdinal(type) || typeReal(type) || type->kind == TYPE_PTR || type->kind == TYPE_STR;
        case TOK_LESSEQ:
        case TOK_GREATEREQ: return typeOrdinal(type) || typeReal(type) || type->kind == TYPE_STR;
        default:            return false;
    }
}


bool typeAssertValidOperator(Types *types, Type *type, TokenKind op)
{
    bool res = typeValidOperator(type, op);
    if (!res)
    {
        char buf[DEFAULT_STR_LEN + 1];
        types->error->handler(types->error->context, "Operator %s is not applicable to %s", lexSpelling(op), typeSpelling(type, buf));
    }
    return res;
}


bool typeAssertForwardResolved(Types *types)
{
    for (Type *type = types->first; type; type = type->next)
        if (type->kind == TYPE_FORWARD)
        {
            types->error->handler(types->error->context, "Unresolved forward declaration of %s", (Ident *)(type->typeIdent)->name);
            return false;
        }
    return true;
}


Field *typeFindField(Type *structType, char *name)
{
    if (structType->kind == TYPE_STRUCT || structType->kind == TYPE_INTERFACE)
    {
        unsigned int nameHash = hash(name);
        for (int i = 0; i < structType->numItems; i++)
            if (structType->field[i]->hash == nameHash && strcmp(structType->field[i]->name, name) == 0)
                return structType->field[i];
    }
    return NULL;
}


Field *typeAssertFindField(Types *types, Type *structType, char *name)
{
    Field *res = typeFindField(structType, name);
    if (!res)
        types->error->handler(types->error->context, "Unknown field %s", name);
    return res;
}


Field *typeAddField(Types *types, Type *structType, Type *fieldType, char *name)
{
    Field *field = typeFindField(structType, name);
    if (field)
        types->error->handler(types->error->context, "Duplicate field %s", name);

    if (fieldType->kind == TYPE_FORWARD)
        types->error->handler(types->error->context, "Unresolved forward type declaration for field %s", name);

    if (fieldType->kind == TYPE_VOID)
        types->error->handler(types->error->context, "Void field %s is not allowed", name);

    if (structType->numItems > MAX_FIELDS)
        types->error->handler(types->error->context, "Too many fields");

    field = malloc(sizeof(Field));

    strcpy(field->name, name);
    field->hash = hash(name);
    field->type = fieldType;
    field->offset = typeSize(types, structType);

    structType->field[structType->numItems++] = field;
    return field;
}


Param *typeFindParam(Signature *sig, char *name)
{
    unsigned int nameHash = hash(name);
    for (int i = 0; i < sig->numParams; i++)
        if (sig->param[i]->hash == nameHash && strcmp(sig->param[i]->name, name) == 0)
            return sig->param[i];

    return NULL;
}


Param *typeAddParam(Types *types, Signature *sig, Type *type, char *name)
{
    Param *param = typeFindParam(sig, name);
    if (param)
        types->error->handler(types->error->context, "Duplicate parameter %s", name);

    if (sig->numParams > MAX_PARAMS)
        types->error->handler(types->error->context, "Too many parameters");

    param = malloc(sizeof(Param));

    strcpy(param->name, name);
    param->hash = hash(name);
    param->type = type;
    param->defaultVal.intVal = 0;

    sig->param[sig->numParams++] = param;
    return param;
}


int typeParamSizeUpTo(Types *types, Signature *sig, int index)
{
    // All parameters are slot-aligned
    int size = 0;
    for (int i = 0; i <= index; i++)
        size += align(typeSize(types, sig->param[i]->type), sizeof(Slot));
    return size;
}


int typeParamSizeTotal(Types *types, Signature *sig)
{
    return typeParamSizeUpTo(types, sig, sig->numParams - 1);
}


char *typeKindSpelling(TypeKind kind)
{
    return spelling[kind];
}


static char *typeSpellingRecursive(Type *type, char *buf, int depth)
{
    if (type->typeIdent)
        sprintf(buf, "%s", type->typeIdent->name);
    else
    {
        if (type->kind == TYPE_ARRAY)
            sprintf(buf, "[%d]", type->numItems);
        else
            sprintf(buf, "%s", spelling[type->kind]);

        if (type->kind == TYPE_PTR || type->kind == TYPE_ARRAY || type->kind == TYPE_DYNARRAY)
        {
            char baseBuf[DEFAULT_STR_LEN + 1];
            if (depth > 0)
                strcat(buf, typeSpellingRecursive(type->base, baseBuf, depth - 1));
            else
                strcat(buf, "...");
        }
    }
    return buf;
}


char *typeSpelling(Type *type, char *buf)
{
    enum {MAX_TYPE_SPELLING_DEPTH = 10};
    return typeSpellingRecursive(type, buf, MAX_TYPE_SPELLING_DEPTH);
}

