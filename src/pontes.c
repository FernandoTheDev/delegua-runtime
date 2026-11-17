#include "interno.h"
/* Compatibilidade das entradas nativas antigas e ABI gerenciada explícita. */
delegua_value delegua_ffi_cgdCurlCriar(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlCriar"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlFechar(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlFechar"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlResetar(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlResetar"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlOpcao(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlOpcao"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlExecutar(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlExecutar"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlLote(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlLote"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlOpcoes(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlOpcoes"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlVersao(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlVersao"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlEscapar(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlEscapar"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlDecodificar(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlDecodificar"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlMultipart(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlMultipart"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlEnviar(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlEnviar"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlReceber(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlReceber"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlEsperar(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlEsperar"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlWsEnviar(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlWsEnviar"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlWsReceber(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlWsReceber"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlInfo(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlInfo"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlUrl(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlUrl"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlDetalhes(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlDetalhes"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlErro(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlErro"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlData(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlData"), argc, argv);
}
delegua_value delegua_ffi_cgdCurlManter(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCurlManter"), argc, argv);
}
delegua_value delegua_ffi_cgdCodigoUnicode(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCodigoUnicode"), argc, argv);
}
delegua_value delegua_ffi_cgdCaractereUnicode(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdCaractereUnicode"), argc, argv);
}
delegua_value delegua_ffi_cgdNumeroFinito(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdNumeroFinito"), argc, argv);
}
delegua_value delegua_ffi_cgdNumeroTexto(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdNumeroTexto"), argc, argv);
}
delegua_value delegua_ffi_cgdArquivoLer(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdArquivoLer"), argc, argv);
}
delegua_value delegua_ffi_cgdArquivoEscrever(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdArquivoEscrever"), argc, argv);
}
delegua_value delegua_ffi_cgdArquivoInfo(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdArquivoInfo"), argc, argv);
}
delegua_value delegua_ffi_cgdArquivoRemover(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdArquivoRemover"), argc, argv);
}
delegua_value delegua_ffi_cgdDicionarioItens(sz argc, delegua_value *argv) {
    return delegua_chamar(delegua_cgd_builtin("cgdDicionarioItens"), argc, argv);
}
