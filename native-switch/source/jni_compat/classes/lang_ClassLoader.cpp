#include <stdio.h>
#include <string.h>
#include <vector>

#include "../../eap_port/platform_adapter.h"
#include "../../eap_port/trace_adapter.h"
#include "../jni.h"
#include "../jni_internals.h"
#include "lang_ClassLoader.h"

static jobject ClassLoader_loadClass(JNIEnv *env, jobject self, jstring jname)
{
    (void)env; (void)self;
    if (!jname) return NULL;
    const char *name = ((String *)jname)->str;
    if (!name) return NULL;
    char path[256];
    size_t i = 0;
    for (; name[i] && i < sizeof(path) - 1; i++)
        path[i] = (name[i] == '.') ? '/' : name[i];
    path[i] = '\0';
    for (const Class *clazz : ClassRegistry::get_class_registry()) {
        if (strcmp(path, clazz->classpath) == 0)
            return (jobject)clazz;
    }
    warning("ClassLoader: no class '%s' in this port.\n", path);
    return NULL;
}

const ManagedMethod langClassLoaderClassMethods[] = {
    ManagedMethod::Register<&ClassLoader_loadClass>(
        LangClassLoader::clazz, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"),
    {NULL},
};

Class LangClassLoader::clazz = {
    .classpath = "java/lang/ClassLoader",
    .classname = "ClassLoader",
    .managed_methods = langClassLoaderClassMethods,
    .native_methods = {NULL},
    .fields = {NULL},
    .instance_size = sizeof(LangClassLoader),
};

LangClassLoader g_class_loader;
static const int registered = ClassRegistry::register_class(LangClassLoader::clazz);