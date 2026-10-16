#include "LicenseManager.h"
#include "Ed25519.h"
#include <algorithm>
#include <regex>
#include <vector>

namespace CassetteLic
{

// Los literales con acentos/ñ son UTF-8; juce::String(const char*) los interpretaría como Latin-1 (mojibake).
#define TXT(s) juce::String (juce::CharPointer_UTF8 (s))

namespace
{
    // Clave pública Ed25519 del backend (PLUGIN_PRIVATE_KEY → pública). Ver tools/gen_ed25519_consts.mjs.
    const std::array<uint8_t, 32> kServerPublicKey = {
        0xfe, 0x7b, 0x0b, 0x45, 0x5a, 0x52, 0x63, 0xfe, 0xe4, 0xcf, 0x01, 0x41, 0x45, 0xfe, 0x83, 0xca,
        0xe1, 0x3f, 0x2f, 0x60, 0x24, 0x46, 0x95, 0xc8, 0x5c, 0x3d, 0x8e, 0x46, 0x16, 0x84, 0x0d, 0xca };

    constexpr const char* kPluginName      = "CASSETTE";
    constexpr const char* kRecordMagic     = "CSLIC2";
    constexpr int64_t     kSeenToleranceS  = 10 * 60;        // margen para pequeños ajustes de reloj (NTP, DST mal configurado)
    constexpr int64_t     kServerToleranceS = 26 * 3600;     // margen frente a la hora del servidor (zonas horarias mal puestas)
    constexpr int64_t     kSeenPersistEveryS = 5 * 60;

    // Sal del sello local. No es un secreto criptográfico fuerte (vive en el binario), sirve para que editar el
    // archivo de licencia a mano o copiarlo a otro equipo lo invalide.
    juce::String sealPepper()
    {
        return juce::String ("OFFSZN::CS::lic::") + "4b9e" + "a1c7" + "e60d" + "|v2";
    }

    std::vector<uint8_t> toBytes (const juce::String& s)
    {
        const auto* p = reinterpret_cast<const uint8_t*> (s.toRawUTF8());
        return std::vector<uint8_t> (p, p + s.getNumBytesAsUTF8());
    }

    juce::String toHex (const uint8_t* data, size_t n)
    {
        return juce::String::toHexString (data, (int) n, 0);
    }

    // HMAC-SHA512 (RFC 2104)
    void hmacSha512 (const std::vector<uint8_t>& key, const std::vector<uint8_t>& msg, uint8_t out[64])
    {
        constexpr size_t block = 128;
        std::vector<uint8_t> k (block, 0);
        if (key.size() > block) crypto::sha512 (key.data(), key.size(), k.data());
        else std::copy (key.begin(), key.end(), k.begin());

        std::vector<uint8_t> inner (block + msg.size()), outer (block + 64);
        for (size_t i = 0; i < block; ++i) { inner[i] = k[i] ^ 0x36; outer[i] = k[i] ^ 0x5c; }
        std::copy (msg.begin(), msg.end(), inner.begin() + block);

        crypto::sha512 (inner.data(), inner.size(), outer.data() + block);
        crypto::sha512 (outer.data(), outer.size(), out);
    }

    bool fromHex (const juce::String& hex, uint8_t* out, size_t n)
    {
        if ((size_t) hex.length() != n * 2) return false;
        for (size_t i = 0; i < n; ++i)
        {
            const int hi = juce::CharacterFunctions::getHexDigitValue (hex[(int) (2 * i)]);
            const int lo = juce::CharacterFunctions::getHexDigitValue (hex[(int) (2 * i + 1)]);
            if (hi < 0 || hi > 15 || lo < 0 || lo > 15) return false;
            out[i] = (uint8_t) (hi * 16 + lo);
        }
        return true;
    }

    // Igual que valida el backend: PREFIJO-(FULL|TRIAL|SUB)-XXXX-XXXX
    juce::String normaliseSerial (const juce::String& raw)
    {
        static const std::regex re ("(EASY|MASTER|INKA|COKE|VOCA|PITCH|OMNI|LEVEL|DEESER|CLEAN|GHOST|CASSETTE)-(FULL|TRIAL|SUB)-[A-Z0-9]{4,8}-[A-Z0-9]{4,8}",
                                    std::regex::icase);
        const std::string s = raw.trim().toUpperCase().toStdString();
        std::smatch m;
        if (std::regex_search (s, m, re))
            return juce::String (m.str (0));
        return {};
    }

    juce::String errorFor (const juce::String& code, const juce::String& serverMsg)
    {
        if (serverMsg.isNotEmpty()) return serverMsg;
        if (code == "expired")      return TXT("Tu periodo de prueba o suscripción ha expirado.");
        if (code == "revoked")      return "Esta licencia fue suspendida o revocada.";
        if (code == "not_found")    return TXT("Licencia no encontrada o inválida.");
        if (code == "device_limit") return TXT("Límite de dispositivos alcanzado para esta licencia.");
        if (code == "trial_used")   return TXT("Este equipo ya utilizó una prueba gratuita anteriormente.");
        return TXT("La clave de licencia no es válida.");
    }

    // Clasifica una negativa del servidor (compatible con backends sin campo `code`).
    juce::String classifyDenial (int status, const juce::String& code, const juce::String& msg)
    {
        if (code.isNotEmpty()) return code;
        const auto m = msg.toLowerCase();
        if (status == 404 || m.contains ("no encontrada")) return "not_found";
        if (m.contains ("expir"))                            return "expired";
        if (m.contains ("revoc") || m.contains ("suspend"))  return "revoked";
        if (m.contains (TXT("límite de dispositivos")))           return "device_limit";
        if (m.contains (TXT("ya utilizó una prueba")))            return "trial_used";
        return {};
    }
}

//==============================================================================
// Etiqueta opcional para builds de pruebas E2E: aíslan archivo y registro de la licencia real del usuario.
#ifndef CASSETTE_STORE_TAG
 #define CASSETTE_STORE_TAG ""
#endif

LicenseEnvironment LicenseEnvironment::makeDefault()
{
    LicenseEnvironment e;
    e.apiBase     = CASSETTE_API_BASE;
    e.primaryFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                        .getChildFile ("OFFSZN").getChildFile (juce::String ("Cassette") + CASSETTE_STORE_TAG + ".lic");
    e.mirrorKey   = juce::String ("Software\\OFFSZN\\Cassette") + CASSETTE_STORE_TAG;
    e.nowFn       = [] { return (int64_t) (juce::Time::currentTimeMillis() / 1000); };
    e.dispatch    = [] (std::function<void()> f) { juce::MessageManager::callAsync (std::move (f)); };
    e.publicKey   = kServerPublicKey;
    return e;
}

juce::String LicenseManager::getDefaultHardwareID()
{
    return juce::SystemStats::getComputerName() + "_" + juce::SystemStats::getUniqueDeviceID();
}

juce::String LicenseManager::getHardwareID() const
{
    const auto name = env.computerName.isNotEmpty() ? env.computerName : juce::SystemStats::getComputerName();
    const auto id   = env.deviceId.isNotEmpty()     ? env.deviceId     : juce::SystemStats::getUniqueDeviceID();
    return name + "_" + id;
}

juce::String LicenseManager::stableId() const
{
    // Solo el id de dispositivo: renombrar el PC no debe invalidar la licencia.
    auto id = env.deviceId.isNotEmpty() ? env.deviceId : juce::SystemStats::getUniqueDeviceID();
    if (id.isEmpty())
        id = env.computerName.isNotEmpty() ? env.computerName : juce::SystemStats::getComputerName();
    return id;
}

juce::String LicenseManager::buildV2Payload (const juce::String& serial, const juce::String& type, const juce::String& expiresStr,
                                             const juce::String& hwid, const juce::String& nonce, int64_t serverTime,
                                             const juce::String& status)
{
    // Debe coincidir con signV2() del backend (PluginLicensingController.js)
    return "v2|" + serial + "|" + type + "|" + expiresStr + "|" + hwid + "|" + nonce + "|" + juce::String (serverTime) + "|" + status;
}

juce::String LicenseManager::makeNonce()
{
    return juce::Uuid().toString();
}

//==============================================================================
LicenseManager::LicenseManager() : LicenseManager (LicenseEnvironment::makeDefault()) {}

LicenseManager::LicenseManager (LicenseEnvironment e)
    : juce::Thread ("Cassette license"), env (std::move (e))
{
    if (! env.nowFn)    env.nowFn = [] { return (int64_t) (juce::Time::currentTimeMillis() / 1000); };
    if (! env.dispatch) env.dispatch = [] (std::function<void()> f) { juce::MessageManager::callAsync (std::move (f)); };

    // Estado inicial SÍNCRONO (lectura de un archivo diminuto): el primer processBlock ya sabe si hay licencia.
    {
        bool allInvalid = false;
        auto r = loadBestRecord (sawAnything, allInvalid);
        const juce::ScopedLock sl (lock);
        record = r;
        sealOk = ! (sawAnything && allInvalid);
        knownFileTime = env.primaryFile.getLastModificationTime();
        lastPersistedSeen = record.lastSeen;
        const auto t = now();
        publishState (record, evaluate (record, sealOk, t), sealOk, sawAnything, t);
    }

    if (env.startWorker)
        startThread (juce::Thread::Priority::low);
}

LicenseManager::~LicenseManager()
{
    signalThreadShouldExit();
    {
        // Si hay una petición HTTP en curso, se cancela para no colgar el cierre del DAW.
        const juce::ScopedLock sl (httpLock);
        if (activeStream != nullptr)
            activeStream->cancel();
    }
    notify();
    stopThread (4000);

    const juce::ScopedLock sl (lock);
    if (record.valid && sealOk)
        persist (record);
}

//==============================================================================
//  Registro sellado
//==============================================================================
juce::String LicenseManager::computeMac (const juce::String& body) const
{
    uint8_t key[64];
    const auto material = toBytes (sealPepper() + "|" + stableId());
    crypto::sha512 (material.data(), material.size(), key);

    uint8_t mac[64];
    hmacSha512 (std::vector<uint8_t> (key, key + 64), toBytes (body), mac);
    return toHex (mac, 32);
}

juce::String LicenseManager::serializeRecord (const Record& r) const
{
    juce::String body;
    body << kRecordMagic << "\n"
         << "serial=" << r.serial << "\n"
         << "type="   << r.type << "\n"
         << "exp="    << juce::String (r.expiresAt) << "\n"
         << "act="    << juce::String (r.activatedAt) << "\n"
         << "seen="   << juce::String (r.lastSeen) << "\n"
         << "srv="    << juce::String (r.serverFloor) << "\n"
         << "chk="    << juce::String (r.lastCheck) << "\n"
         << "st="     << r.status;
    return body + "\nmac=" + computeMac (body) + "\n";
}

bool LicenseManager::readRecordFrom (const juce::String& text, Record& out) const
{
    if (text.isEmpty()) return false;

    juce::StringArray lines;
    lines.addLines (text);

    if (lines.size() < 2 || lines[0].trim() != kRecordMagic) return false;

    juce::StringPairArray kv;
    for (int i = 1; i < lines.size(); ++i)
    {
        const auto ln = lines[i];
        const int eq = ln.indexOfChar ('=');
        if (eq > 0) kv.set (ln.substring (0, eq).trim(), ln.substring (eq + 1).trim());
    }

    Record r;
    r.serial      = kv["serial"];
    r.type        = kv["type"];
    r.expiresAt   = kv["exp"].getLargeIntValue();
    r.activatedAt = kv["act"].getLargeIntValue();
    r.lastSeen    = kv["seen"].getLargeIntValue();
    r.serverFloor = kv["srv"].getLargeIntValue();
    r.lastCheck   = kv["chk"].getLargeIntValue();
    r.status      = kv["st"].isEmpty() ? "active" : kv["st"];
    r.valid       = r.serial.isNotEmpty();

    // El sello se recalcula sobre la forma canónica: cualquier edición (o copia de otro equipo) lo rompe.
    const auto canonical = serializeRecord (r);
    const auto expected = canonical.fromLastOccurrenceOf ("\nmac=", false, false).trim();
    if (! r.valid || expected != kv["mac"].toLowerCase())
        return false;

    out = r;
    return true;
}

juce::String LicenseManager::readMirror() const
{
    if (! env.useMirror) return {};
   #if JUCE_WINDOWS
    return juce::WindowsRegistry::getValue ("HKEY_CURRENT_USER\\" + env.mirrorKey + "\\lic", {});
   #else
    auto f = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".offszn")
                 .getChildFile (juce::File::createLegalFileName (env.mirrorKey) + ".lic");
    return f.existsAsFile() ? f.loadFileAsString() : juce::String();
   #endif
}

void LicenseManager::writeMirror (const juce::String& text) const
{
    if (! env.useMirror) return;
   #if JUCE_WINDOWS
    juce::WindowsRegistry::setValue ("HKEY_CURRENT_USER\\" + env.mirrorKey + "\\lic", text);
   #else
    auto f = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".offszn")
                 .getChildFile (juce::File::createLegalFileName (env.mirrorKey) + ".lic");
    f.getParentDirectory().createDirectory();
    f.replaceWithText (text);
   #endif
}

LicenseManager::Record LicenseManager::loadBestRecord (bool& sawAny, bool& allInvalid)
{
    const juce::String texts[2] = { env.primaryFile.existsAsFile() ? env.primaryFile.loadFileAsString() : juce::String(),
                                    readMirror() };
    Record best;
    sawAny = false;
    bool anyValid = false;

    for (const auto& t : texts)
    {
        if (t.trim().isEmpty()) continue;
        sawAny = true;
        Record r;
        if (readRecordFrom (t, r) && (! anyValid || r.lastSeen > best.lastSeen))
        {
            best = r;
            anyValid = true;
        }
    }

    allInvalid = sawAny && ! anyValid;

    // Autocuración: si falta o difiere alguna de las dos copias, se reescribe la buena.
    if (anyValid)
    {
        const auto canon = serializeRecord (best);
        if (texts[0] != canon || texts[1] != canon)
            persist (best);
    }
    return best;
}

void LicenseManager::persist (const Record& r)
{
    const auto text = serializeRecord (r);
    env.primaryFile.getParentDirectory().createDirectory();
    env.primaryFile.replaceWithText (text);
    writeMirror (text);
    knownFileTime = env.primaryFile.getLastModificationTime();
    lastPersistedSeen = r.lastSeen;
}

//==============================================================================
//  Evaluación de estado
//==============================================================================
LicenseManager::Evaluation LicenseManager::evaluate (const Record& r, bool seal, int64_t t) const
{
    Evaluation e;
    if (! r.valid)
    {
        e.reason = seal ? LockReason::NoLicense : LockReason::Tampered;
        return e;
    }
    if (! seal)                       { e.reason = LockReason::Tampered; return e; }
    if (r.status == "revoked")        { e.reason = LockReason::Revoked;  return e; }
    if (r.status == "expired")        { e.reason = LockReason::Expired;  return e; }

    // Anti-reloj: el tiempo nunca puede ir hacia atrás respecto a lo ya observado (local o del servidor).
    if (t + kSeenToleranceS < r.lastSeen || t + kServerToleranceS < r.serverFloor)
    {
        e.reason = LockReason::ClockTampered;
        return e;
    }

    // "Ahora" efectivo: nunca menos que lo último visto → mover el reloj unos minutos atrás no regala tiempo.
    const int64_t eff = std::max ({ t, r.lastSeen, r.serverFloor });

    if (r.expiresAt > 0)
    {
        e.secondsLeft = r.expiresAt - eff;
        if (e.secondsLeft <= 0)
        {
            e.secondsLeft = 0;
            e.reason = LockReason::Expired;
            return e;
        }
        e.daysLeft = (int) ((e.secondsLeft + 86399) / 86400);
    }

    e.valid = true;
    e.reason = LockReason::None;
    return e;
}

juce::String LicenseManager::maskSerial (const juce::String& s) const
{
    // CASSETTE-FULL-ABCD1234-EFGH5678  →  CASSETTE-FULL-AB••••••-••••••78
    auto parts = juce::StringArray::fromTokens (s, "-", "");
    if (parts.size() != 4) return s.isEmpty() ? s : juce::String::repeatedString (TXT("•"), 8);
    auto g1 = parts[2], g2 = parts[3];
    g1 = g1.substring (0, 2) + juce::String::repeatedString (juce::String (juce::CharPointer_UTF8 ("\xE2\x80\xA2")), juce::jmax (0, g1.length() - 2));
    g2 = juce::String::repeatedString (juce::String (juce::CharPointer_UTF8 ("\xE2\x80\xA2")), juce::jmax (0, g2.length() - 2))
         + g2.substring (juce::jmax (0, g2.length() - 2));
    return parts[0] + "-" + parts[1] + "-" + g1 + "-" + g2;
}

void LicenseManager::publishState (const Record& r, const Evaluation& e, bool seal, bool sawAny, int64_t)
{
    LicenseState s;
    s.isValid    = e.valid;
    s.lockReason = e.reason;
    s.daysLeft   = e.daysLeft;
    s.secondsLeft = e.secondsLeft;
    s.expiresAt  = r.expiresAt;
    s.lastCheck  = r.lastCheck;
    s.serial     = seal && r.valid ? maskSerial (r.serial) : juce::String();

    const auto t = r.type.toLowerCase();
    if (r.valid && seal)
        s.kind = t == "trial" ? LicenseKind::Trial
               : (t == "subscription" || t == "sub") ? LicenseKind::Subscription
               : LicenseKind::Full;
    s.isTrial = s.kind == LicenseKind::Trial || (s.kind == LicenseKind::Subscription && r.expiresAt > 0);

    switch (e.reason)
    {
        case LockReason::None:
            s.code = s.kind == LicenseKind::Trial ? "trial" : (s.kind == LicenseKind::Subscription ? "subscription" : "full");
            s.message = s.kind == LicenseKind::Full ? "Licencia FULL activa."
                      : "Prueba activa: " + juce::String (e.daysLeft) + (e.daysLeft == 1 ? TXT(" día restante.") : TXT(" días restantes."));
            break;
        case LockReason::NoLicense:
            s.code = "none";
            s.message = TXT("Activa tu licencia o inicia la prueba gratis de 3 días.");
            break;
        case LockReason::Expired:
            s.code = "expired";
            s.message = s.kind == LicenseKind::Trial ? "Tu periodo de prueba ha finalizado."
                                                     : TXT("Tu suscripción ha vencido.");
            break;
        case LockReason::Revoked:
            s.code = "revoked";
            s.message = TXT("Esta licencia fue revocada o ya no es válida para este equipo.");
            break;
        case LockReason::Tampered:
            s.code = "tampered";
            s.message = TXT("No se pudo verificar la licencia en este equipo. Actívala de nuevo con conexión a internet.");
            break;
        case LockReason::ClockTampered:
            s.code = "clock";
            s.message = TXT("La fecha y hora del sistema es anterior a la última vez que se usó el plugin. Corrígela para continuar.");
            break;
    }
    (void) sawAny;

    const bool changed = cached.code != s.code || cached.daysLeft != s.daysLeft || cached.isValid != s.isValid
                      || cached.serial != s.serial || cached.kind != s.kind || cached.lastCheck != s.lastCheck
                      || cached.expiresAt != s.expiresAt;
    cached = s;
    licensed.store (s.isValid, std::memory_order_relaxed);
    if (changed) ++stateVersion;
}

LicenseState LicenseManager::getLicenseState() const
{
    const juce::ScopedLock sl (lock);
    return cached;
}

//==============================================================================
//  Hilo de mantenimiento
//==============================================================================
void LicenseManager::run()
{
    while (! threadShouldExit())
    {
        drainJobs();
        if (threadShouldExit()) break;
        maintenanceTick();
        wait (5000);
    }
}

void LicenseManager::requestRevalidation()
{
    {
        const juce::ScopedLock sl (lock);
        revalidateRequested = true;
        nextAuditAttempt = 0;
    }
    notify();
}

void LicenseManager::maintenanceTick()
{
    // 1) Otro proceso/instancia pudo haber activado o revocado: recargar si el archivo cambió.
    if (env.primaryFile.getLastModificationTime() != knownFileTime)
    {
        bool any = false, bad = false;
        auto r = loadBestRecord (any, bad);
        const juce::ScopedLock sl (lock);
        if (r.valid || any)
        {
            record = r;
            sealOk = ! (any && bad);
            sawAnything = any;
        }
        knownFileTime = env.primaryFile.getLastModificationTime();
    }

    Record r;
    bool seal;
    {
        const juce::ScopedLock sl (lock);
        r = record;
        seal = sealOk;
    }

    const auto t = now();
    const auto e = evaluate (r, seal, t);

    // 2) Avanzar la marca de "último instante visto" (solo si el reloj es coherente).
    if (r.valid && seal && e.reason != LockReason::ClockTampered && e.reason != LockReason::Tampered)
    {
        if (t > r.lastSeen)
        {
            r.lastSeen = t;
            const juce::ScopedLock sl (lock);
            if (record.serial == r.serial)
            {
                record.lastSeen = std::max (record.lastSeen, r.lastSeen);
                if (record.lastSeen - lastPersistedSeen >= kSeenPersistEveryS)
                    persist (record);
            }
        }
    }

    {
        const juce::ScopedLock sl (lock);
        publishState (record, evaluate (record, sealOk, t), sealOk, sawAnything, t);
    }

    // 3) Auditoría silenciosa cada 5 h (o cuando se pide). Nunca en caliente para usuarios sin registro.
    bool due = false;
    {
        const juce::ScopedLock sl (lock);
        if (record.valid && sealOk)
            due = (revalidateRequested || (t - record.lastCheck >= env.checkIntervalSec)) && t >= nextAuditAttempt;
    }
    if (due && ! threadShouldExit())
        doAudit();
}

//==============================================================================
//  Red
//==============================================================================
HttpResponse LicenseManager::post (const juce::String& path, const juce::var& body)
{
    const auto url = env.apiBase + path;
    const auto json = juce::JSON::toString (body, true);

    if (env.httpPost)
        return env.httpPost (url, json, 8000);

    HttpResponse resp;
    juce::URL u = juce::URL (url).withPOSTData (json);
    auto wis = std::make_unique<juce::WebInputStream> (u, true);
    wis->withExtraHeaders ("Content-Type: application/json\r\nAccept: application/json\r\nUser-Agent: Cassette/1.0\r\n");
    wis->withConnectionTimeout (8000);
    wis->withNumRedirectsToFollow (2);

    {
        const juce::ScopedLock sl (httpLock);
        activeStream = wis.get();
    }

    if (wis->connect (nullptr))
    {
        resp.reached = true;
        resp.status  = wis->getStatusCode();
        resp.body    = wis->readEntireStreamAsString();
    }

    {
        const juce::ScopedLock sl (httpLock);
        activeStream = nullptr;
    }
    return resp;
}

bool LicenseManager::verifyResponse (const juce::var& json, const juce::String& serial, const juce::String& hwid,
                                     const juce::String& nonce, juce::String& why) const
{
    if (env.signaturePolicy <= 0) return true;

    const auto sigV2 = json.getProperty ("signature_v2", {}).toString().trim();
    const auto sigV1 = json.getProperty ("signature", {}).toString().trim();

    uint8_t sig[64];
    const auto expiresStr = json.getProperty ("expires_at", "never").toString();

    if (sigV2.isNotEmpty() && sigV2 != "no-signature")
    {
        if (! fromHex (sigV2, sig, 64)) { why = "firma mal formada"; return false; }

        const auto payload = buildV2Payload (serial, json.getProperty ("license_type", {}).toString(), expiresStr, hwid, nonce,
                                             (int64_t) json.getProperty ("server_time", 0),
                                             json.getProperty ("status", "active").toString());
        const auto bytes = toBytes (payload);
        if (crypto::ed25519Verify (sig, bytes.data(), bytes.size(), env.publicKey.data()))
            return true;
        why = TXT("firma v2 inválida");
        return false;
    }

    if (sigV1.isNotEmpty() && sigV1 != "no-signature")
    {
        if (! fromHex (sigV1, sig, 64)) { why = "firma mal formada"; return false; }
        const auto bytes = toBytes (serial + "|" + expiresStr);
        if (crypto::ed25519Verify (sig, bytes.data(), bytes.size(), env.publicKey.data()))
            return true;
        why = TXT("firma v1 inválida");
        return false;
    }

    // Sin firma: solo se acepta en la política 1 (backend sin actualizar).
    if (env.signaturePolicy >= 2) { why = "respuesta sin firma"; return false; }
    return true;
}

namespace
{
enum class Verdict { Granted, Denied, Inconclusive };

// Resultado interpretado de una respuesta del servidor
struct InterpretedReply
{
    Verdict verdict = Verdict::Inconclusive;
    juce::String code;
    juce::String message;
    juce::var    json;
    int          status = 0;
};

InterpretedReply interpret (const HttpResponse& resp)
{
    InterpretedReply r;
    r.status = resp.status;
    if (! resp.reached)
    {
        r.code = "offline";
        r.message = TXT("No se pudo conectar con OFFSZN. Revisa tu conexión a internet e inténtalo de nuevo.");
        return r;
    }

    r.json = juce::JSON::parse (resp.body);
    if (! r.json.isObject())
    {
        r.code = "bad_response";
        r.message = "Respuesta inesperada del servidor (HTTP " + juce::String (resp.status) + TXT("). Inténtalo más tarde.");
        return r;
    }

    const bool ok = (bool) r.json.getProperty ("success", false) || (bool) r.json.getProperty ("valid", false);
    if (ok)
    {
        r.verdict = Verdict::Granted;
        return r;
    }

    if (resp.status >= 500)
    {
        r.code = "server_error";
        r.message = TXT("El servidor no está disponible en este momento. Inténtalo más tarde.");
        return r;
    }

    const auto msg  = r.json.getProperty ("error", r.json.getProperty ("message", {})).toString();
    const auto code = classifyDenial (resp.status, r.json.getProperty ("code", {}).toString(), msg);

    // Un "no" del servidor. Para la auditoría solo ciertos códigos bloquean; para la activación todos se muestran.
    r.verdict = Verdict::Denied;
    r.code = code.isEmpty() ? "rejected" : code;
    r.message = errorFor (r.code, msg);
    return r;
}
} // namespace

ActivationResult LicenseManager::doActivate (const juce::String& rawSerial)
{
    ActivationResult out;
    const auto serial = normaliseSerial (rawSerial);
    if (serial.isEmpty())
    {
        out.code = "bad_format";
        out.message = rawSerial.trim().isEmpty() ? "Por favor ingresa tu clave de licencia."
                                                 : TXT("Formato de clave no válido. Ejemplo: CASSETTE-FULL-XXXXXXXX-XXXXXXXX");
        return out;
    }

    const auto hwid  = getHardwareID();
    const auto nonce = makeNonce();

    auto* body = new juce::DynamicObject();
    body->setProperty ("serial_key", serial);
    body->setProperty ("hwid", hwid);
    body->setProperty ("device_id", stableId());
    body->setProperty ("device_name", env.computerName.isNotEmpty() ? env.computerName : juce::SystemStats::getComputerName());
    body->setProperty ("plugin_name", kPluginName);
    body->setProperty ("nonce", nonce);

    const auto reply = interpret (post ("/api/plugin/activate", juce::var (body)));

    if (reply.verdict != Verdict::Granted)
    {
        out.code = reply.code;
        out.message = reply.message;
        return out;
    }

    // Una respuesta con éxito pero para OTRA clave, o con firma inválida, se rechaza.
    juce::String why;
    const auto respSerial = reply.json.getProperty ("serial_key", serial).toString().toUpperCase();
    if (respSerial != serial || ! verifyResponse (reply.json, serial, hwid, nonce, why))
    {
        out.code = "bad_signature";
        out.message = TXT("No se pudo verificar la autenticidad de la respuesta del servidor. Comprueba tu conexión (proxy/VPN/hosts) e inténtalo de nuevo.");
        return out;
    }

    const auto t = now();
    const auto serverTime = (int64_t) reply.json.getProperty ("server_time", 0);

    Record r;
    r.serial      = serial;
    r.type        = reply.json.getProperty ("license_type", "lifetime").toString().toLowerCase();
    r.expiresAt   = (int64_t) reply.json.getProperty ("expires_at_unix", 0);
    r.status      = "active";
    r.valid       = true;
    r.lastCheck   = t;

    {
        const juce::ScopedLock sl (lock);
        const bool same = record.valid && record.serial == serial;
        r.activatedAt = same ? record.activatedAt : t;
        // La historia del reloj se conserva entre activaciones: reactivar no "limpia" un retroceso de fecha.
        r.lastSeen    = std::max (t, record.valid ? record.lastSeen : (int64_t) 0);
        r.serverFloor = std::max (record.valid ? record.serverFloor : 0, serverTime);
        record = r;
        sealOk = true;
        sawAnything = true;
        persist (record);
        publishState (record, evaluate (record, true, t), true, true, t);
    }

    out.success = true;
    out.code = "ok";
    out.message = r.type == "trial" ? TXT("¡Prueba activada! Disfruta CASSETTE durante 3 días.")
                                    : TXT("¡Activación completada con éxito!");
    return out;
}

ActivationResult LicenseManager::doStartTrial()
{
    ActivationResult out;
    const auto hwid = getHardwareID();

    auto* body = new juce::DynamicObject();
    body->setProperty ("hwid", hwid);
    body->setProperty ("device_id", stableId());
    body->setProperty ("device_name", env.computerName.isNotEmpty() ? env.computerName : juce::SystemStats::getComputerName());
    body->setProperty ("plugin_name", kPluginName);

    const auto reply = interpret (post ("/api/plugin/request-trial", juce::var (body)));
    if (reply.verdict != Verdict::Granted)
    {
        out.code = (bool) reply.json.getProperty ("trial_expired", false) ? "trial_expired" : reply.code;
        out.message = reply.message;
        return out;
    }

    // request-trial entrega la clave de prueba; la activación real (con firma anti-falsificación) va por /activate.
    return doActivate (reply.json.getProperty ("serial_key", {}).toString());
}

void LicenseManager::doAudit()
{
    Record r;
    {
        const juce::ScopedLock sl (lock);
        r = record;
    }
    const auto t0 = now();
    const auto hwid = getHardwareID();
    const auto nonce = makeNonce();

    auto* body = new juce::DynamicObject();
    body->setProperty ("serial_key", r.serial);
    body->setProperty ("hwid", hwid);
    body->setProperty ("device_id", stableId());
    body->setProperty ("plugin_name", kPluginName);
    body->setProperty ("nonce", nonce);
    const juce::var bodyVar (body);

    auto reply = interpret (post ("/api/plugin/validate", bodyVar));

    // Backend antiguo sin /validate (404 HTML): /activate es idempotente para equipos ya registrados.
    bool usedActivate = false;
    if (reply.verdict == Verdict::Inconclusive && reply.code == "bad_response" && (reply.status == 404 || reply.status == 405))
    {
        reply = interpret (post ("/api/plugin/activate", bodyVar));
        usedActivate = true;
    }
    (void) usedActivate;

    const auto t = now();
    const juce::ScopedLock sl (lock);

    auto retryLater = [&] { nextAuditAttempt = t + env.retryIntervalSec; revalidateRequested = false; };

    if (reply.verdict == Verdict::Granted)
    {
        juce::String why;
        if (! verifyResponse (reply.json, r.serial, hwid, nonce, why))
        {
            retryLater();          // respuesta no auténtica: se ignora (no bloquea al usuario, no renueva la auditoría)
            return;
        }

        const auto serverTime = (int64_t) reply.json.getProperty ("server_time", 0);
        record.expiresAt   = (int64_t) reply.json.getProperty ("expires_at_unix", record.expiresAt);
        record.type        = reply.json.getProperty ("license_type", record.type).toString().toLowerCase();
        record.status      = "active";
        record.lastCheck   = t;
        record.serverFloor = std::max (record.serverFloor, serverTime);
        record.lastSeen    = std::max (record.lastSeen, t0);
        nextAuditAttempt   = 0;
        revalidateRequested = false;
        persist (record);
    }
    else if (reply.verdict == Verdict::Denied
             && (reply.code == "revoked" || reply.code == "not_found" || reply.code == "device_not_registered"))
    {
        record.status = "revoked";
        revalidateRequested = false;
        persist (record);
    }
    else if (reply.verdict == Verdict::Denied && reply.code == "expired")
    {
        record.status = "expired";
        revalidateRequested = false;
        persist (record);
    }
    else
    {
        retryLater();              // sin red / error del servidor / inconcluso: seguir con la licencia local
        return;
    }

    publishState (record, evaluate (record, sealOk, t), sealOk, sawAnything, t);
}

//==============================================================================
//  Acciones públicas
//==============================================================================
void LicenseManager::activate (const juce::String& serial, std::function<void (ActivationResult)> callback)
{
    {
        const juce::ScopedLock sl (jobLock);
        jobs.push_back ({ Job::Kind::Activate, serial, std::move (callback) });
    }
    if (env.startWorker) notify();
}

void LicenseManager::startTrial (std::function<void (ActivationResult)> callback)
{
    {
        const juce::ScopedLock sl (jobLock);
        jobs.push_back ({ Job::Kind::Trial, {}, std::move (callback) });
    }
    if (env.startWorker) notify();
}

void LicenseManager::drainJobs()
{
    for (;;)
    {
        Job job;
        {
            const juce::ScopedLock sl (jobLock);
            if (jobs.empty()) return;
            job = std::move (jobs.front());
            jobs.pop_front();
        }

        const auto result = job.kind == Job::Kind::Trial ? doStartTrial() : doActivate (job.serial);

        if (job.callback)
        {
            auto cb = std::move (job.callback);
            env.dispatch ([cb = std::move (cb), result] { cb (result); });
        }
    }
}

} // namespace CassetteLic
