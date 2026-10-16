#pragma once
#include <cstddef>
#include <cstdint>

// Verificación Ed25519 (RFC 8032), sin dependencias ni asignaciones dinámicas.
// Aritmética de campo basada en TweetNaCl (dominio público). Se usa únicamente para comprobar que las
// respuestas de activación las firmó el backend de OFFSZN y no un servidor falso (hosts/proxy).
namespace CassetteLic::crypto
{
    // true si `sig` (64 bytes) es una firma Ed25519 válida de `msg` con la clave pública `pk` (32 bytes).
    bool ed25519Verify (const uint8_t sig[64], const uint8_t* msg, size_t msgLen, const uint8_t pk[32]);

    // SHA-512 (usado por Ed25519; expuesto para los tests).
    void sha512 (const uint8_t* data, size_t len, uint8_t out[64]);
}
