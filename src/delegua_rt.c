/*
 * Delegua Runtime
 *
 * Copyright 2026 Fernando the Dev
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "delegua_rt.h"
#include "assincrono.h"

// ponto de entrada
int main(int argc, char *argv[]) {
    cgd_gc_init();
    cgd_gc_allow_threads();
    (void)delegua_classe_objeto();
    (void)delegua_classe_excecao();
    program_create(&context, argc, argv);
    delegua_try_frame *root = delegua_tentar();
    volatile bool failed = false;
    volatile int status = EXIT_SUCCESS;
    if (setjmp(root->env) == 0) {
        delegua_main();
        status = rt_async_program_finish();
    } else
        failed = true;
    delegua_sair_tente(root);
    if (failed)
        status = rt_async_program_abort(root->error);
    program_free(&context);
    cgd_gc_shutdown();
    return status;
}
