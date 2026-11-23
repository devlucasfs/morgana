#pragma once
extern "C" {
    typedef void SamarState;
    SamarState *__samar_load(char *file);
    void __samar_push_arg(SamarState *state, char *name, char *value);
    void __samar_call_runner(SamarState *state, char *runner);
}
