#include <nullvm.h>
#include <string.h>

typedef union IntValue {
    jint i;
    jlong j;
    Env* env;
    void* ptr;
} IntValue;

typedef union FpValue {
    jdouble d;
    jfloat f;
} FpValue;

typedef union StackValue {
    jdouble d;
    jfloat f;
    jint i;
    jlong j;
    Env* env;
    void* ptr;
} StackValue;

typedef struct CallInfo {
    void* function;
    void* intArgs[6];
    double fpArgs[8];
    jint stackArgsCount;
    StackValue* stackArgs;
} CallInfo;

extern void _nvmCall0(CallInfo*);

Method* nvmGetMethod(Env* env, Class* clazz, char* name, char* desc) {
    Method* method;
    for (method = clazz->methods; method != NULL; method = method->next) {
        if (!strcmp(method->name, name) && !strcmp(method->desc, desc)) {
            return method;
        }
    }

    if (clazz->superclass && strcmp("<init>", name) && strcmp("<clinit>", name)) {
        /* 
         * Check with the superclass. Note that constructors and static 
         * initializers are not inherited.
         */
        return nvmGetMethod(env, clazz->superclass, name, desc);
    }

    nvmThrowNoSuchMethodError(env, name);
    return NULL;
}

Method* nvmGetClassMethod(Env* env, Class* clazz, char* name, char* desc) {
    Method* method = nvmGetMethod(env, clazz, name, desc);
    if (!method) return NULL;
    if (!(method->access & ACC_STATIC)) {
        // TODO: JNI spec doesn't say anything about throwing this
        nvmThrowIncompatibleClassChangeErrorMethod(env, clazz, name, desc);
        return NULL;
    }
    return method;
}

Method* nvmGetInstanceMethod(Env* env, Class* clazz, char* name, char* desc) {
    Method* method = nvmGetMethod(env, clazz, name, desc);
    if (!method) return NULL;
    if (method->access & ACC_STATIC) {
        // TODO: JNI spec doesn't say anything about throwing this
        nvmThrowIncompatibleClassChangeErrorMethod(env, clazz, name, desc);
        return NULL;
    }
    return method;
}

static char getNextType(char** desc) {
    char c = **desc;
    (*desc)++;
    switch (c) {
    case 'B':
    case 'Z':
    case 'S':
    case 'C':
    case 'I':
    case 'J':
    case 'F':
    case 'D':
        return c;
    case '[':
        getNextType(desc);
        return c;
    case 'L':
        while (**desc != ';') (*desc)++;
        (*desc)++;
        return c;
    case '(':
        return getNextType(desc);
    }
    return 0;
}

static inline jboolean isIntType(char type) {
    return type == 'B' || type == 'Z' || type == 'S' || type == 'C' || type == 'I' || type == 'J' || type == 'L' || type == '[';
}

static inline jboolean isFpType(char type) {
    return type == 'F' || type == 'D';
}

jboolean initCallInfo(CallInfo* callInfo, Env* env, Class* clazz, Object* obj, Method* method, jboolean virtual, jvalue* args) {
    if (virtual && !(method->access & ACC_PRIVATE)) {
        // Lookup the real method to be invoked
        method = nvmGetMethod(env, obj->clazz, method->name, method->desc);
        if (!method) return FALSE;
    }

    jint argsCount = 0, intArgsCount = 0, fpArgsCount = 0, stackArgsCount = 0;

    intArgsCount = 2; // First arg is a Invoke(Static|Virtual|Special|Interface)* which we ignore
                      // Second arg is always the Env*
    if (!(method->access & ACC_STATIC)) {
        // Non-static methods takes the receiver object (this) as arg 3
        intArgsCount++;
    }    

    char* desc = method->desc;
    char c;
    while (c = getNextType(&desc)) {
        argsCount++;
        if (isFpType(c) && fpArgsCount < 8) {
            fpArgsCount++;
        } else if (intArgsCount < 6) {
            intArgsCount++;
        } else {
            stackArgsCount++;
        }
    }

    callInfo->function = method->impl;
    callInfo->stackArgsCount = stackArgsCount;
    if (stackArgsCount > 0) {
        callInfo->stackArgs = nvmAllocateMemory(env, sizeof(StackValue) * stackArgsCount);
        if (!callInfo->stackArgs) return FALSE;
    }

    jint intArgsIndex = 0, fpArgsIndex = 0, stackArgsIndex = 0;

    callInfo->intArgs[intArgsIndex++] = NULL;
    callInfo->intArgs[intArgsIndex++] = env;
    if (!(method->access & ACC_STATIC)) {
        callInfo->intArgs[intArgsIndex++] = obj;
    }    

    desc = method->desc;
    jint i = 0;
    while (c = getNextType(&desc)) {
        if (isFpType(c)) {
            if (fpArgsIndex < fpArgsCount) {
                switch (c) {
                case 'F':
                    callInfo->fpArgs[fpArgsIndex++] = (double) args[i++].f;
                    break;
                case 'D':
                    callInfo->fpArgs[fpArgsIndex++] = args[i++].d;
                    break;
                }
            } else {
                switch (c) {
                case 'F':
                    callInfo->stackArgs[stackArgsIndex++].f = args[i++].f;
                    break;
                case 'D':
                    callInfo->stackArgs[stackArgsIndex++].d = args[i++].d;
                    break;
                }
            }
        } else {
            if (intArgsIndex < intArgsCount) {
                callInfo->intArgs[intArgsIndex++] = (void*) args[i++].j;
            } else {
                callInfo->stackArgs[stackArgsIndex++].j = args[i++].j;
            }
        }
    }

    return TRUE;
}

static jvalue* va_list2jargs(Env* env, Method* method, va_list args) {
    jint argsCount = 0;
    char* desc = method->desc;
    char c;
    while (c = getNextType(&desc)) {
        argsCount++;
    }

    jvalue *jvalueArgs = (jvalue*) nvmAllocateMemory(env, sizeof(jvalue) * argsCount);
    if (!jvalueArgs) return NULL;

    desc = method->desc;
    jint i = 0;
    while (c = getNextType(&desc)) {
        switch (c) {
        case 'B':
            jvalueArgs[i++].b = (jbyte) va_arg(args, jint);
            break;
        case 'Z':
            jvalueArgs[i++].z = (jboolean) va_arg(args, jint);
            break;
        case 'S':
            jvalueArgs[i++].s = (jshort) va_arg(args, jint);
            break;
        case 'C':
            jvalueArgs[i++].c = (jchar) va_arg(args, jint);
            break;
        case 'I':
            jvalueArgs[i++].i = va_arg(args, jint);
            break;
        case 'J':
            jvalueArgs[i++].j = va_arg(args, jlong);
            break;
        case 'F':
            jvalueArgs[i++].f = (jdouble) va_arg(args, jdouble);
            break;
        case 'D':
            jvalueArgs[i++].d = va_arg(args, jdouble);
            break;
        case '[':
        case 'L':
            jvalueArgs[i++].l = va_arg(args, jobject);
            break;
        }
    }

    return jvalueArgs;
}

void nvmCallVoidInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, TRUE, args)) return;
    void (*f)(CallInfo*) = _nvmCall0;
    f(&callInfo);
}

void nvmCallVoidInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return;
    nvmCallVoidInstanceMethodA(env, obj, method, jargs);
}

void nvmCallVoidInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    nvmCallVoidInstanceMethodV(env, obj, method, args);
}

jboolean nvmCallBooleanInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, TRUE, args)) return FALSE;
    jboolean (*f)(CallInfo*) = (jboolean (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jboolean nvmCallBooleanInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return FALSE;
    return nvmCallBooleanInstanceMethodA(env, obj, method, jargs);
}

jboolean nvmCallBooleanInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallBooleanInstanceMethodV(env, obj, method, args);
}

jbyte nvmCallByteInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, TRUE, args)) return 0;
    jbyte (*f)(CallInfo*) = (jbyte (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jbyte nvmCallByteInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallByteInstanceMethodA(env, obj, method, jargs);
}

jbyte nvmCallByteInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallByteInstanceMethodV(env, obj, method, args);
}

jchar nvmCallCharInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, TRUE, args)) return 0;
    jchar (*f)(CallInfo*) = (jchar (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jchar nvmCallCharInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallCharInstanceMethodA(env, obj, method, jargs);
}

jchar nvmCallCharInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallCharInstanceMethodV(env, obj, method, args);
}

jshort nvmCallShortInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, TRUE, args)) return 0;
    jshort (*f)(CallInfo*) = (jshort (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jshort nvmCallShortInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallShortInstanceMethodA(env, obj, method, jargs);
}

jshort nvmCallShortInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallShortInstanceMethodV(env, obj, method, args);
}

jint nvmCallIntInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, TRUE, args)) return 0;
    jint (*f)(CallInfo*) = (jint (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jint nvmCallIntInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallIntInstanceMethodA(env, obj, method, jargs);
}

jint nvmCallIntInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallIntInstanceMethodV(env, obj, method, args);
}

jlong nvmCallLongInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, TRUE, args)) return 0;
    jlong (*f)(CallInfo*) = (jlong (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jlong nvmCallLongInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallLongInstanceMethodA(env, obj, method, jargs);
}

jlong nvmCallLongInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallLongInstanceMethodV(env, obj, method, args);
}

jfloat nvmCallFloatInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, TRUE, args)) return 0.0f;
    jfloat (*f)(CallInfo*) = (jfloat (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jfloat nvmCallFloatInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallFloatInstanceMethodA(env, obj, method, jargs);
}

jfloat nvmCallFloatInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallFloatInstanceMethodV(env, obj, method, args);
}

jdouble nvmCallDoubleInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, TRUE, args)) return 0.0;
    jdouble (*f)(CallInfo*) = (jdouble (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jdouble nvmCallDoubleInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallDoubleInstanceMethodA(env, obj, method, jargs);
}

jdouble nvmCallDoubleInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallDoubleInstanceMethodV(env, obj, method, args);
}

void nvmCallNonvirtualVoidInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, FALSE, args)) return;
    void (*f)(CallInfo*) = _nvmCall0;
    f(&callInfo);
}

void nvmCallNonvirtualVoidInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return;
    nvmCallNonvirtualVoidInstanceMethodA(env, obj, method, jargs);
}

void nvmCallNonvirtualVoidInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    nvmCallNonvirtualVoidInstanceMethodV(env, obj, method, args);
}

jboolean nvmCallNonvirtualBooleanInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, FALSE, args)) return FALSE;
    jboolean (*f)(CallInfo*) = (jboolean (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jboolean nvmCallNonvirtualBooleanInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return FALSE;
    return nvmCallNonvirtualBooleanInstanceMethodA(env, obj, method, jargs);
}

jboolean nvmCallNonvirtualBooleanInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallNonvirtualBooleanInstanceMethodV(env, obj, method, args);
}

jbyte nvmCallNonvirtualByteInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, FALSE, args)) return 0;
    jbyte (*f)(CallInfo*) = (jbyte (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jbyte nvmCallNonvirtualByteInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallNonvirtualByteInstanceMethodA(env, obj, method, jargs);
}

jbyte nvmCallNonvirtualByteInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallNonvirtualByteInstanceMethodV(env, obj, method, args);
}

jchar nvmCallNonvirtualCharInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, FALSE, args)) return 0;
    jchar (*f)(CallInfo*) = (jchar (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jchar nvmCallNonvirtualCharInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallNonvirtualCharInstanceMethodA(env, obj, method, jargs);
}

jchar nvmCallNonvirtualCharInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallNonvirtualCharInstanceMethodV(env, obj, method, args);
}

jshort nvmCallNonvirtualShortInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, FALSE, args)) return 0;
    jshort (*f)(CallInfo*) = (jshort (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jshort nvmCallNonvirtualShortInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallNonvirtualShortInstanceMethodA(env, obj, method, jargs);
}

jshort nvmCallNonvirtualShortInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallNonvirtualShortInstanceMethodV(env, obj, method, args);
}

jint nvmCallNonvirtualIntInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, FALSE, args)) return 0;
    jint (*f)(CallInfo*) = (jint (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jint nvmCallNonvirtualIntInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallNonvirtualIntInstanceMethodA(env, obj, method, jargs);
}

jint nvmCallNonvirtualIntInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallNonvirtualIntInstanceMethodV(env, obj, method, args);
}

jlong nvmCallNonvirtualLongInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, FALSE, args)) return 0;
    jlong (*f)(CallInfo*) = (jlong (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jlong nvmCallNonvirtualLongInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallNonvirtualLongInstanceMethodA(env, obj, method, jargs);
}

jlong nvmCallNonvirtualLongInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallNonvirtualLongInstanceMethodV(env, obj, method, args);
}

jfloat nvmCallNonvirtualFloatInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, FALSE, args)) return 0.0f;
    jfloat (*f)(CallInfo*) = (jfloat (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jfloat nvmCallNonvirtualFloatInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallNonvirtualFloatInstanceMethodA(env, obj, method, jargs);
}

jfloat nvmCallNonvirtualFloatInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallNonvirtualFloatInstanceMethodV(env, obj, method, args);
}

jdouble nvmCallNonvirtualDoubleInstanceMethodA(Env* env, Object* obj, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, obj->clazz, obj, method, FALSE, args)) return 0.0;
    jdouble (*f)(CallInfo*) = (jdouble (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jdouble nvmCallNonvirtualDoubleInstanceMethodV(Env* env, Object* obj, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallNonvirtualDoubleInstanceMethodA(env, obj, method, jargs);
}

jdouble nvmCallNonvirtualDoubleInstanceMethod(Env* env, Object* obj, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallNonvirtualDoubleInstanceMethodV(env, obj, method, args);
}

void nvmCallVoidClassMethodA(Env* env, Class* clazz, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, clazz, NULL, method, FALSE, args)) return;
    void (*f)(CallInfo*) = _nvmCall0;
    f(&callInfo);
}

void nvmCallVoidClassMethodV(Env* env, Class* clazz, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return;
    nvmCallVoidClassMethodA(env, NULL, method, jargs);
}

void nvmCallVoidClassMethod(Env* env, Class* clazz, Method* method, ...) {
    va_list args;
    va_start(args, method);
    nvmCallVoidClassMethodV(env, NULL, method, args);
}

jboolean nvmCallBooleanClassMethodA(Env* env, Class* clazz, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, clazz, NULL, method, FALSE, args)) return FALSE;
    jboolean (*f)(CallInfo*) = (jboolean (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jboolean nvmCallBooleanClassMethodV(Env* env, Class* clazz, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return FALSE;
    return nvmCallBooleanClassMethodA(env, NULL, method, jargs);
}

jboolean nvmCallBooleanClassMethod(Env* env, Class* clazz, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallBooleanClassMethodV(env, NULL, method, args);
}

jbyte nvmCallByteClassMethodA(Env* env, Class* clazz, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, clazz, NULL, method, FALSE, args)) return 0;
    jbyte (*f)(CallInfo*) = (jbyte (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jbyte nvmCallByteClassMethodV(Env* env, Class* clazz, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallByteClassMethodA(env, NULL, method, jargs);
}

jbyte nvmCallByteClassMethod(Env* env, Class* clazz, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallByteClassMethodV(env, NULL, method, args);
}

jchar nvmCallCharClassMethodA(Env* env, Class* clazz, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, clazz, NULL, method, FALSE, args)) return 0;
    jchar (*f)(CallInfo*) = (jchar (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jchar nvmCallCharClassMethodV(Env* env, Class* clazz, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallCharClassMethodA(env, NULL, method, jargs);
}

jchar nvmCallCharClassMethod(Env* env, Class* clazz, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallCharClassMethodV(env, NULL, method, args);
}

jshort nvmCallShortClassMethodA(Env* env, Class* clazz, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, clazz, NULL, method, FALSE, args)) return 0;
    jshort (*f)(CallInfo*) = (jshort (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jshort nvmCallShortClassMethodV(Env* env, Class* clazz, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallShortClassMethodA(env, NULL, method, jargs);
}

jshort nvmCallShortClassMethod(Env* env, Class* clazz, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallShortClassMethodV(env, NULL, method, args);
}

jint nvmCallIntClassMethodA(Env* env, Class* clazz, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, clazz, NULL, method, FALSE, args)) return 0;
    jint (*f)(CallInfo*) = (jint (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jint nvmCallIntClassMethodV(Env* env, Class* clazz, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallIntClassMethodA(env, NULL, method, jargs);
}

jint nvmCallIntClassMethod(Env* env, Class* clazz, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallIntClassMethodV(env, NULL, method, args);
}

jlong nvmCallLongClassMethodA(Env* env, Class* clazz, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, clazz, NULL, method, FALSE, args)) return 0;
    jlong (*f)(CallInfo*) = (jlong (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jlong nvmCallLongClassMethodV(Env* env, Class* clazz, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallLongClassMethodA(env, NULL, method, jargs);
}

jlong nvmCallLongClassMethod(Env* env, Class* clazz, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallLongClassMethodV(env, NULL, method, args);
}

jfloat nvmCallFloatClassMethodA(Env* env, Class* clazz, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, clazz, NULL, method, FALSE, args)) return 0.0f;
    jfloat (*f)(CallInfo*) = (jfloat (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jfloat nvmCallFloatClassMethodV(Env* env, Class* clazz, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallFloatClassMethodA(env, NULL, method, jargs);
}

jfloat nvmCallFloatClassMethod(Env* env, Class* clazz, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallFloatClassMethodV(env, NULL, method, args);
}

jdouble nvmCallDoubleClassMethodA(Env* env, Class* clazz, Method* method, jvalue* args) {
    CallInfo callInfo = {0};
    if (!initCallInfo(&callInfo, env, clazz, NULL, method, FALSE, args)) return 0.0;
    jdouble (*f)(CallInfo*) = (jdouble (*)(CallInfo*)) _nvmCall0;
    return f(&callInfo);
}

jdouble nvmCallDoubleClassMethodV(Env* env, Class* clazz, Method* method, va_list args) {
    jvalue* jargs = va_list2jargs(env, method, args);
    if (!jargs) return 0;
    return nvmCallDoubleClassMethodA(env, NULL, method, jargs);
}

jdouble nvmCallDoubleClassMethod(Env* env, Class* clazz, Method* method, ...) {
    va_list args;
    va_start(args, method);
    return nvmCallDoubleClassMethodV(env, NULL, method, args);
}


/*
Method* nvmGetMethod(Env* env, Class* clazz, char* name, char* desc) {
    Method* method;
    int sameClass = caller == NULL || clazz == caller;
    int subClass = caller == NULL || nvmIsSubClass(clazz, caller);
    int samePackage = caller == NULL || nvmIsSamePackage(clazz, caller);

    for (method = clazz->methods; method != NULL; method = method->next) {
        if (!strcmp(method->name, name) && !strcmp(method->desc, desc)) {
            jint access = method->access;
            if (IS_PRIVATE(access) && !sameClass) {
                nvmThrowIllegalAccessErrorMethod(clazz, name, desc, caller);
            }
            if (IS_PROTECTED(access) && !subClass) {
                nvmThrowIllegalAccessErrorMethod(clazz, name, desc, caller);
            }
            if (IS_PACKAGE_PRIVATE(access) && !samePackage) {
                nvmThrowIllegalAccessErrorMethod(clazz, name, desc, caller);
            }
            return method;
        }
    }

    if (clazz->superclass && strcmp("<init>", name) && strcmp("<clinit>", name)) {
        /* 
         * Check with the superclass. Note that constructors and static 
         * initializers are not inherited.
         */
       /* return nvmGetMethod(clazz, name, desc);
    }

    nvmThrowNoSuchMethodError(name);
}*/

void* nvmGetNativeMethod(Env* env, char* shortMangledName, char* longMangledName) {
    void* handle = dlopen(NULL, RTLD_LAZY);
    LOG("Searching for native method using short name: %s\n", shortMangledName);
    void* f = dlsym(handle, shortMangledName);
    if (!f) {
        LOG("Searching for native method using long name: %s\n", longMangledName);
        f = dlsym(handle, longMangledName);
        if (f) {
            LOG("Found native method using long name: %s\n", longMangledName);
        }
    } else {
        LOG("Found native method using short name: %s\n", shortMangledName);
    }
    dlclose(handle);
    if (!f) {
        nvmThrowUnsatisfiedLinkError(env);
        return NULL;
    }
    return f;
}

