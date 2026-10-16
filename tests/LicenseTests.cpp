// CassetteLicenseTests — licenciamiento, anti-abuso y DSP base. Sin DAW, sin red real, sin esperar días:
// reloj, red y disco se inyectan (LicenseEnvironment). Los tests de firma usan tools/sign_test.mjs (necesita Node).
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Ed25519.h"
#include "LicenseManager.h"
#include <cmath>
#include <iostream>

using namespace CassetteLic;

static int gChecks = 0, gFailed = 0;
#define CHECK(cond) do { ++gChecks; if (!(cond)) { ++gFailed; std::cout << "    FALLO  " << #cond << "  (línea " << __LINE__ << ")\n"; } } while (0)
#define CHECK_EQ(a, b) do { ++gChecks; if (!((a) == (b))) { ++gFailed; std::cout << "    FALLO  " << #a << " == " << #b << "  -> '" << (a) << "' vs '" << (b) << "'  (línea " << __LINE__ << ")\n"; } } while (0)

static constexpr int64_t DAY = 86400;
static const juce::String kSerialFull  = "CASSETTE-FULL-ABCD1234-EFGH5678";
static const juce::String kSerialTrial = "CASSETTE-TRIAL-AAAA1111-BBBB2222";

//==============================================================================
//  Firmante de prueba (Node)
//==============================================================================
static juce::String runNode (const juce::StringArray& args)
{
    juce::ChildProcess p;
    juce::StringArray cmd { "node", juce::File (CASSETTE_TOOLS_DIR).getChildFile ("sign_test.mjs").getFullPathName() };
    cmd.addArray (args);
    if (! p.start (cmd)) return {};
    auto out = p.readAllProcessOutput().trim();
    p.waitForProcessToFinish (5000);
    return out;
}

static std::array<uint8_t, 32> testPublicKey()
{
    std::array<uint8_t, 32> k {};
    auto hex = runNode ({ "pub" });
    for (int i = 0; i < 32 && hex.length() >= 64; ++i)
        k[(size_t) i] = (uint8_t) (juce::CharacterFunctions::getHexDigitValue (hex[2 * i]) * 16 + juce::CharacterFunctions::getHexDigitValue (hex[2 * i + 1]));
    return k;
}

//==============================================================================
//  Servidor simulado
//==============================================================================
struct FakeServer
{
    // Configuración
    bool reachable = true;
    int  httpStatus = 200;
    juce::String denyCode;          // si no está vacío: 403 con este código
    juce::String denyMessage;
    juce::String licenseType = "lifetime";
    int64_t expiresAt = 0;
    int64_t* clock = nullptr;
    int64_t serverTimeOffset = 0;   // server_time = ahora + offset
    bool sign = false;
    bool htmlOn404Validate = false; // backend antiguo sin /validate
    juce::String signNonceOverride; // simula reutilizar una respuesta vieja (nonce distinto)
    juce::String tamperExpires;     // altera expires_at DESPUÉS de firmar
    bool omitValidateSuccess = false;

    // Contadores
    int calls = 0, activateCalls = 0, validateCalls = 0, trialCalls = 0;

    HttpResponse handle (const juce::String& url, const juce::String& body)
    {
        ++calls;
        HttpResponse r;
        if (! reachable) return r;

        const bool isActivate = url.endsWith ("/activate");
        const bool isValidate = url.endsWith ("/validate");
        const bool isTrial    = url.endsWith ("/request-trial");
        if (isActivate) ++activateCalls;
        if (isValidate) ++validateCalls;
        if (isTrial)    ++trialCalls;

        r.reached = true;
        r.status = httpStatus;
        if (httpStatus >= 500) { r.body = "{\"error\":\"boom\"}"; return r; }
        if (isValidate && htmlOn404Validate) { r.status = 404; r.body = "<html>Cannot POST /api/plugin/validate</html>"; return r; }

        auto req = juce::JSON::parse (body);
        if (isTrial)
        {
            r.body = "{\"success\":true,\"serial_key\":\"" + kSerialTrial + "\",\"license_type\":\"trial\"}";
            return r;
        }

        if (denyCode.isNotEmpty())
        {
            r.status = denyCode == "not_found" ? 404 : 403;
            r.body = "{\"error\":\"" + (denyMessage.isEmpty() ? juce::String ("denegado") : denyMessage) + "\",\"code\":\"" + denyCode + "\"}";
            return r;
        }

        const juce::String serial = req.getProperty ("serial_key", {}).toString();
        const juce::String hwid   = req.getProperty ("hwid", {}).toString();
        const juce::String nonce  = req.getProperty ("nonce", {}).toString();
        const int64_t serverTime  = *clock + serverTimeOffset;
        const juce::String expStr = expiresAt > 0 ? juce::Time (expiresAt * 1000).toISO8601 (true) : juce::String ("never");

        auto* o = new juce::DynamicObject();
        o->setProperty ("success", true);
        o->setProperty ("valid", true);
        o->setProperty ("serial_key", serial);
        o->setProperty ("license_type", licenseType);
        o->setProperty ("expires_at", expStr);
        o->setProperty ("expires_at_unix", (juce::int64) expiresAt);
        o->setProperty ("server_time", (juce::int64) serverTime);
        o->setProperty ("nonce", nonce);
        o->setProperty ("status", "active");
        if (sign)
        {
            const auto payload = LicenseManager::buildV2Payload (serial, licenseType, expStr, hwid,
                                                                 signNonceOverride.isNotEmpty() ? signNonceOverride : nonce,
                                                                 serverTime, "active");
            o->setProperty ("signature_v2", runNode ({ "sign", payload }));
        }
        if (tamperExpires.isNotEmpty()) o->setProperty ("expires_at", tamperExpires);
        r.body = juce::JSON::toString (juce::var (o), true);
        return r;
    }
};

//==============================================================================
//  Fixture
//==============================================================================
struct Fixture
{
    juce::File dir;
    int64_t clock = 1800000000;    // reloj simulado (segundos)
    FakeServer server;
    juce::String deviceId = "DEVICE-AAAA-1111-2222";
    juce::String computerName = "PC-DE-WILLIE";
    int policy = 0;
    bool useMirror = false;
    juce::String mirrorKey;
    std::array<uint8_t, 32> pub {};

    Fixture (const juce::String& name)
    {
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("cassette_tests").getChildFile (name);
        dir.deleteRecursively();
        dir.createDirectory();
        server.clock = &clock;
        mirrorKey = "Software\\OFFSZN\\CassetteTest_" + name;
    }

    ~Fixture()
    {
        dir.deleteRecursively();
       #if JUCE_WINDOWS
        juce::WindowsRegistry::deleteKey ("HKEY_CURRENT_USER\\" + mirrorKey);
       #endif
    }

    juce::File licFile() const { return dir.getChildFile ("Cassette.lic"); }

    LicenseEnvironment env() const
    {
        LicenseEnvironment e;
        e.apiBase = "https://test.invalid";
        e.primaryFile = licFile();
        e.useMirror = useMirror;
        e.mirrorKey = mirrorKey;
        e.nowFn = [this] { return clock; };
        e.httpPost = [this] (const juce::String& url, const juce::String& body, int) { return const_cast<FakeServer&> (server).handle (url, body); };
        e.dispatch = [] (std::function<void()> f) { f(); };
        e.deviceId = deviceId;
        e.computerName = computerName;
        e.signaturePolicy = policy;
        e.publicKey = pub;
        e.startWorker = false;
        return e;
    }

    std::unique_ptr<LicenseManager> make() const { return std::make_unique<LicenseManager> (env()); }

    static ActivationResult activate (LicenseManager& m, const juce::String& serial)
    {
        ActivationResult res;
        m.activate (serial, [&res] (ActivationResult r) { res = r; });
        m.runPendingJobsForTests();
        return res;
    }

    static ActivationResult trial (LicenseManager& m)
    {
        ActivationResult res;
        m.startTrial ([&res] (ActivationResult r) { res = r; });
        m.runPendingJobsForTests();
        return res;
    }
};

#define TEST(name) static void name(); static struct Reg_##name { Reg_##name() { registry().push_back ({ #name, &name }); } } reg_##name; static void name()
struct TestCase { const char* name; void (*fn)(); };
static std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }

//==============================================================================
//  Criptografía
//==============================================================================
TEST (ed25519_verifica_firma_real_del_backend)
{
    // Vector generado con PLUGIN_PRIVATE_KEY de producción (tools/gen_ed25519_consts.mjs)
    const uint8_t pk[32] = { 0xfe,0x7b,0x0b,0x45,0x5a,0x52,0x63,0xfe,0xe4,0xcf,0x01,0x41,0x45,0xfe,0x83,0xca,
                             0xe1,0x3f,0x2f,0x60,0x24,0x46,0x95,0xc8,0x5c,0x3d,0x8e,0x46,0x16,0x84,0x0d,0xca };
    const juce::String payload = "PITCH-FULL-AAAA1111-BBBB2222|never";   // vector fijo firmado por el backend real (no cambiar)
    const juce::String sigHex = "dc8b02649da6f343f9a42a42053ab8bab729744fc88066a1bc2aceabd8a9eafa5e947d1fcd1f926de58eca05678dde65e966d95379c474bd8781c0aef391810f";
    uint8_t sig[64];
    for (int i = 0; i < 64; ++i)
        sig[i] = (uint8_t) (juce::CharacterFunctions::getHexDigitValue (sigHex[2 * i]) * 16 + juce::CharacterFunctions::getHexDigitValue (sigHex[2 * i + 1]));

    const auto* m = reinterpret_cast<const uint8_t*> (payload.toRawUTF8());
    CHECK (crypto::ed25519Verify (sig, m, (size_t) payload.getNumBytesAsUTF8(), pk));

    const auto bad = payload + "x";
    CHECK (! crypto::ed25519Verify (sig, reinterpret_cast<const uint8_t*> (bad.toRawUTF8()), (size_t) bad.getNumBytesAsUTF8(), pk));
    sig[3] ^= 0x01;
    CHECK (! crypto::ed25519Verify (sig, m, (size_t) payload.getNumBytesAsUTF8(), pk));

    uint8_t h[64];
    crypto::sha512 (reinterpret_cast<const uint8_t*> ("abc"), 3, h);
    CHECK (h[0] == 0xdd && h[1] == 0xaf && h[2] == 0x35 && h[3] == 0xa1);
}

//==============================================================================
//  Activación y estados
//==============================================================================
TEST (instalacion_limpia_sin_licencia)
{
    Fixture f ("cassette");
    auto m = f.make();
    CHECK (! m->isLicensed());
    CHECK_EQ (m->getLicenseState().code, juce::String ("none"));
}

TEST (activacion_full_y_funciona_offline_para_siempre)
{
    Fixture f ("full");
    f.server.licenseType = "lifetime";
    {
        auto m = f.make();
        auto r = Fixture::activate (*m, kSerialFull);
        CHECK (r.success);
        CHECK (m->isLicensed());
        CHECK_EQ (m->getLicenseState().code, juce::String ("full"));
        CHECK (! m->getLicenseState().isTrial);
        CHECK (m->getLicenseState().serial.contains ("CASSETTE-FULL-AB"));
        CHECK (! m->getLicenseState().serial.contains ("EFGH5678"));   // enmascarado
    }
    f.server.reachable = false;                    // sin internet, un año después, otra sesión de DAW
    f.clock += 400 * DAY;
    auto m2 = f.make();
    m2->runMaintenanceTickForTests();
    CHECK (m2->isLicensed());
    CHECK_EQ (m2->getLicenseState().code, juce::String ("full"));
}

TEST (activacion_requiere_internet)
{
    Fixture f ("offline_activate");
    f.server.reachable = false;
    auto m = f.make();
    auto r = Fixture::activate (*m, kSerialFull);
    CHECK (! r.success);
    CHECK_EQ (r.code, juce::String ("offline"));
    CHECK (! m->isLicensed());
    CHECK (! f.licFile().existsAsFile());          // no se escribe nada sin confirmación del servidor
}

TEST (formato_invalido_no_llega_al_servidor)
{
    Fixture f ("badformat");
    auto m = f.make();
    CHECK_EQ (Fixture::activate (*m, "hola mundo").code, juce::String ("bad_format"));
    CHECK_EQ (Fixture::activate (*m, "").code, juce::String ("bad_format"));
    CHECK_EQ (f.server.calls, 0);
    // pegar "Cassette: CASSETTE-FULL-..." con texto alrededor también funciona
    CHECK (Fixture::activate (*m, "  Cassette: clean-full-abcd1234-efgh5678 (mi clave) ").success);
}

TEST (prueba_cuenta_regresiva_3_2_1_expirado)
{
    Fixture f ("trial");
    f.server.licenseType = "trial";
    f.server.expiresAt = f.clock + 3 * DAY;
    auto m = f.make();
    CHECK (Fixture::activate (*m, kSerialTrial).success);

    auto st = m->getLicenseState();
    CHECK (st.isValid && st.isTrial);
    CHECK_EQ (st.code, juce::String ("trial"));
    CHECK_EQ (st.daysLeft, 3);

    f.clock += 1 * DAY + 60;   m->runMaintenanceTickForTests();
    CHECK_EQ (m->getLicenseState().daysLeft, 2);

    f.clock += 1 * DAY;        m->runMaintenanceTickForTests();
    CHECK_EQ (m->getLicenseState().daysLeft, 1);
    CHECK (m->isLicensed());

    f.clock += 23 * 3600;      m->runMaintenanceTickForTests();     // quedan ~1 h - 2 min
    CHECK (m->isLicensed());
    CHECK_EQ (m->getLicenseState().daysLeft, 1);

    f.clock += 2 * 3600;       m->runMaintenanceTickForTests();     // ya venció
    CHECK (! m->isLicensed());
    CHECK_EQ (m->getLicenseState().code, juce::String ("expired"));

    // un plugin nuevo (otro arranque del DAW) también lo ve expirado
    f.server.reachable = false;
    auto m2 = f.make();
    CHECK (! m2->isLicensed());
    CHECK_EQ (m2->getLicenseState().code, juce::String ("expired"));
}

TEST (prueba_expirada_se_desbloquea_con_full)
{
    Fixture f ("trial_to_full");
    f.server.licenseType = "trial";
    f.server.expiresAt = f.clock + 3 * DAY;
    auto m = f.make();
    CHECK (Fixture::activate (*m, kSerialTrial).success);
    f.clock += 4 * DAY;
    m->runMaintenanceTickForTests();
    CHECK (! m->isLicensed());

    f.server.licenseType = "lifetime";
    f.server.expiresAt = 0;
    CHECK (Fixture::activate (*m, kSerialFull).success);
    CHECK (m->isLicensed());
    CHECK_EQ (m->getLicenseState().code, juce::String ("full"));
}

TEST (iniciar_prueba_gratis_desde_el_plugin)
{
    Fixture f ("start_trial");
    f.server.licenseType = "trial";
    f.server.expiresAt = f.clock + 3 * DAY;
    auto m = f.make();
    auto r = Fixture::trial (*m);
    CHECK (r.success);
    CHECK_EQ (f.server.trialCalls, 1);
    CHECK_EQ (f.server.activateCalls, 1);
    CHECK_EQ (m->getLicenseState().daysLeft, 3);
}

//==============================================================================
//  Anti-abuso
//==============================================================================
TEST (reloj_atrasado_bloquea_y_se_recupera)
{
    Fixture f ("clock");
    f.server.licenseType = "trial";
    f.server.expiresAt = f.clock + 3 * DAY;
    auto m = f.make();
    CHECK (Fixture::activate (*m, kSerialTrial).success);

    f.clock += 2 * DAY;  m->runMaintenanceTickForTests();       // usa el plugin 2 días
    CHECK (m->isLicensed());

    const int64_t real = f.clock;
    f.clock = real - 2 * DAY;                                   // retrocede el reloj para "ganar" tiempo
    m->runMaintenanceTickForTests();
    CHECK (! m->isLicensed());
    CHECK_EQ (m->getLicenseState().code, juce::String ("clock"));

    auto m2 = f.make();                                         // reiniciar el DAW no lo arregla
    CHECK (! m2->isLicensed());
    CHECK_EQ (m2->getLicenseState().code, juce::String ("clock"));

    f.clock = real + 60;                                        // corrige el reloj → vuelve solo
    m2->runMaintenanceTickForTests();
    CHECK (m2->isLicensed());
}

TEST (retroceso_pequeno_de_reloj_se_tolera_pero_no_regala_tiempo)
{
    Fixture f ("clock_small");
    f.server.licenseType = "trial";
    f.server.expiresAt = f.clock + 3 * DAY;
    auto m = f.make();
    CHECK (Fixture::activate (*m, kSerialTrial).success);
    f.clock += 2 * DAY + 23 * 3600;   m->runMaintenanceTickForTests();     // ~1 h de prueba restante
    f.clock -= 5 * 60;                m->runMaintenanceTickForTests();     // ajuste de 5 min (NTP)
    CHECK (m->isLicensed());
    f.clock += 3 * 3600;              m->runMaintenanceTickForTests();
    CHECK (! m->isLicensed());
}

TEST (editar_el_archivo_de_licencia_lo_invalida)
{
    Fixture f ("edit");
    f.server.licenseType = "trial";
    f.server.expiresAt = f.clock + 3 * DAY;
    { auto m = f.make(); CHECK (Fixture::activate (*m, kSerialTrial).success); }

    auto text = f.licFile().loadFileAsString();
    const auto edited = text.replace ("exp=" + juce::String ((juce::int64) f.server.expiresAt), "exp=" + juce::String ((juce::int64) f.server.expiresAt + 3650 * DAY));
    CHECK (edited != text);
    f.licFile().replaceWithText (edited);

    auto m2 = f.make();
    CHECK (! m2->isLicensed());
    CHECK_EQ (m2->getLicenseState().code, juce::String ("tampered"));

    f.licFile().replaceWithText ("EASY-FULL-AAAA-BBBB");        // formato viejo/inventado
    auto m3 = f.make();
    CHECK (! m3->isLicensed());

    // se recupera reactivando con internet
    CHECK (Fixture::activate (*m3, kSerialTrial).success);
    CHECK (m3->isLicensed());
}

TEST (copiar_la_licencia_a_otro_equipo_no_funciona)
{
    Fixture f ("copy");
    { auto m = f.make(); CHECK (Fixture::activate (*m, kSerialFull).success); }
    f.deviceId = "OTRO-EQUIPO-9999-8888";
    auto m2 = f.make();
    CHECK (! m2->isLicensed());
    CHECK_EQ (m2->getLicenseState().code, juce::String ("tampered"));
}

TEST (renombrar_el_pc_no_invalida_la_licencia)
{
    Fixture f ("rename");
    { auto m = f.make(); CHECK (Fixture::activate (*m, kSerialFull).success); }
    f.computerName = "NOMBRE-NUEVO";
    auto m2 = f.make();
    CHECK (m2->isLicensed());
    CHECK (m2->getHardwareID().startsWith ("NOMBRE-NUEVO_"));
}

TEST (borrar_el_archivo_no_regala_prueba_y_la_copia_secundaria_lo_cura)
{
   #if JUCE_WINDOWS
    Fixture f ("mirror");
    f.useMirror = true;
    { auto m = f.make(); CHECK (Fixture::activate (*m, kSerialFull).success); }

    f.licFile().deleteFile();                                   // "limpiador" o usuario borra el archivo
    auto m2 = f.make();
    CHECK (m2->isLicensed());                                   // el registro de Windows conserva la copia
    CHECK (f.licFile().existsAsFile());                         // y el archivo se regenera

    // borrar AMBAS copias = sin licencia (nunca un trial nuevo)
    f.licFile().deleteFile();
    juce::WindowsRegistry::deleteKey ("HKEY_CURRENT_USER\\" + f.mirrorKey);
    auto m3 = f.make();
    CHECK (! m3->isLicensed());
    CHECK_EQ (m3->getLicenseState().code, juce::String ("none"));
   #endif
}

TEST (auditoria_cada_5_horas_no_antes)
{
    Fixture f ("audit_interval");
    { auto m = f.make(); CHECK (Fixture::activate (*m, kSerialFull).success); }
    auto m = f.make();
    f.server.validateCalls = 0;

    f.clock += 4 * 3600 + 59 * 60;  m->runMaintenanceTickForTests();
    CHECK_EQ (f.server.validateCalls, 0);
    f.clock += 2 * 60;              m->runMaintenanceTickForTests();
    CHECK_EQ (f.server.validateCalls, 1);
    f.clock += 1 * 3600;            m->runMaintenanceTickForTests();     // acaba de auditar: no repite
    CHECK_EQ (f.server.validateCalls, 1);
    f.clock += 5 * 3600;            m->runMaintenanceTickForTests();
    CHECK_EQ (f.server.validateCalls, 2);
    CHECK (m->isLicensed());
}

TEST (auditoria_revocacion_bloquea_incluso_tras_reiniciar_offline)
{
    Fixture f ("audit_revoke");
    { auto m = f.make(); CHECK (Fixture::activate (*m, kSerialFull).success); }
    auto m = f.make();
    f.server.denyCode = "revoked";
    f.clock += 5 * 3600 + 10;       m->runMaintenanceTickForTests();
    CHECK (! m->isLicensed());
    CHECK_EQ (m->getLicenseState().code, juce::String ("revoked"));

    f.server.reachable = false;                                       // apagar internet no lo revierte
    auto m2 = f.make();
    CHECK (! m2->isLicensed());
    CHECK_EQ (m2->getLicenseState().code, juce::String ("revoked"));
}

TEST (auditoria_fallos_de_red_o_servidor_nunca_bloquean_al_usuario)
{
    Fixture f ("audit_offline");
    { auto m = f.make(); CHECK (Fixture::activate (*m, kSerialFull).success); }
    auto m = f.make();
    f.server.validateCalls = 0;

    f.server.reachable = false;
    f.clock += 5 * 3600 + 10;       m->runMaintenanceTickForTests();
    CHECK (m->isLicensed());
    CHECK_EQ (f.server.calls > 0, true);

    f.server.reachable = true; f.server.httpStatus = 500;
    f.clock += 31 * 60;             m->runMaintenanceTickForTests();     // reintenta a los 30 min
    CHECK (m->isLicensed());

    f.server.httpStatus = 200; f.server.htmlOn404Validate = false;
    f.clock += 31 * 60;             m->runMaintenanceTickForTests();
    CHECK (m->isLicensed());
}

TEST (auditoria_reintenta_cada_30_min_no_cada_5_segundos)
{
    Fixture f ("audit_retry");
    { auto m = f.make(); CHECK (Fixture::activate (*m, kSerialFull).success); }
    auto m = f.make();
    f.server.reachable = false;
    f.clock += 5 * 3600 + 10;
    int before = f.server.calls;
    m->runMaintenanceTickForTests();
    const int afterFirst = f.server.calls;
    CHECK (afterFirst > before);
    for (int i = 0; i < 50; ++i) { f.clock += 5; m->runMaintenanceTickForTests(); }   // ~4 min de ticks
    CHECK_EQ (f.server.calls, afterFirst);
    f.clock += 30 * 60;  m->runMaintenanceTickForTests();
    CHECK (f.server.calls > afterFirst);
}

TEST (auditoria_extiende_suscripcion_renovada)
{
    Fixture f ("audit_extend");
    f.server.licenseType = "subscription";
    f.server.expiresAt = f.clock + 2 * DAY;
    auto m = f.make();
    CHECK (Fixture::activate (*m, "CASSETTE-SUB-ABCD1234-EFGH5678").success);
    CHECK_EQ (m->getLicenseState().code, juce::String ("subscription"));
    CHECK_EQ (m->getLicenseState().daysLeft, 2);

    f.server.expiresAt = f.clock + 32 * DAY;                        // renovó en la web
    f.clock += 5 * 3600 + 10;       m->runMaintenanceTickForTests();
    CHECK_EQ (m->getLicenseState().daysLeft, 32 - 0);               // 31 d 19 h → 32
}

TEST (backend_antiguo_sin_validate_usa_activate_como_respaldo)
{
    Fixture f ("legacy_backend");
    { auto m = f.make(); CHECK (Fixture::activate (*m, kSerialFull).success); }
    auto m = f.make();
    f.server.htmlOn404Validate = true;
    f.server.activateCalls = 0;
    f.clock += 5 * 3600 + 10;       m->runMaintenanceTickForTests();
    CHECK_EQ (f.server.activateCalls, 1);
    CHECK (m->isLicensed());
    f.server.denyCode = "revoked";                                   // y el respaldo también puede revocar
    f.clock += 5 * 3600 + 10;       m->runMaintenanceTickForTests();
    CHECK (! m->isLicensed());
}

TEST (reloj_local_muy_atrasado_frente_al_servidor_pide_corregirlo)
{
    Fixture f ("server_time");
    f.server.serverTimeOffset = 2 * DAY;                             // el servidor dice que estamos 2 días en el pasado
    auto m = f.make();
    CHECK (Fixture::activate (*m, kSerialFull).success);
    CHECK (! m->isLicensed());
    CHECK_EQ (m->getLicenseState().code, juce::String ("clock"));
    f.server.serverTimeOffset = 0;
    f.clock += 2 * DAY;                                              // arregla la hora
    m->runMaintenanceTickForTests();
    CHECK (m->isLicensed());
}

TEST (errores_del_servidor_se_traducen_a_codigos)
{
    Fixture f ("errors");
    auto m = f.make();
    f.server.denyCode = "expired";        CHECK_EQ (Fixture::activate (*m, kSerialTrial).code, juce::String ("expired"));
    f.server.denyCode = "revoked";        CHECK_EQ (Fixture::activate (*m, kSerialFull).code, juce::String ("revoked"));
    f.server.denyCode = "not_found";      CHECK_EQ (Fixture::activate (*m, kSerialFull).code, juce::String ("not_found"));
    f.server.denyCode = "device_limit";    CHECK_EQ (Fixture::activate (*m, kSerialFull).code, juce::String ("device_limit"));
    f.server.denyCode = "trial_used";     CHECK_EQ (Fixture::activate (*m, kSerialTrial).code, juce::String ("trial_used"));
    f.server.denyCode = {};
    f.server.httpStatus = 503;            CHECK_EQ (Fixture::activate (*m, kSerialFull).code, juce::String ("server_error"));
    CHECK (! m->isLicensed());
    CHECK (! f.licFile().existsAsFile());
}

//==============================================================================
//  Firmas (servidor falso / repetición)
//==============================================================================
TEST (firmas_v2_aceptadas_y_ataques_rechazados)
{
    Fixture f ("signatures");
    f.pub = testPublicKey();
    f.policy = 2;
    f.server.sign = true;
    f.server.licenseType = "trial";
    f.server.expiresAt = f.clock + 3 * DAY;

    {   // legítima
        auto m = f.make();
        auto r = Fixture::activate (*m, kSerialTrial);
        CHECK (r.success);
        CHECK (m->isLicensed());
    }
    f.licFile().deleteFile();
    {   // servidor falso: éxito sin firma
        f.server.sign = false;
        auto m = f.make();
        auto r = Fixture::activate (*m, kSerialFull);
        CHECK (! r.success);
        CHECK_EQ (r.code, juce::String ("bad_signature"));
        CHECK (! m->isLicensed());
    }
    {   // repetición: firma válida pero de OTRA petición (nonce distinto)
        f.server.sign = true;
        f.server.signNonceOverride = "nonce-de-una-respuesta-capturada";
        auto m = f.make();
        CHECK_EQ (Fixture::activate (*m, kSerialTrial).code, juce::String ("bad_signature"));
        CHECK (! m->isLicensed());
    }
    {   // alterar la expiración en tránsito (proxy) invalida la firma
        f.server.signNonceOverride = {};
        f.server.tamperExpires = "2099-01-01T00:00:00.000Z";
        auto m = f.make();
        CHECK_EQ (Fixture::activate (*m, kSerialTrial).code, juce::String ("bad_signature"));
        CHECK (! m->isLicensed());
    }
    {   // clave pública equivocada (backend distinto)
        f.server.tamperExpires = {};
        f.pub[0] ^= 0xff;
        auto m = f.make();
        CHECK_EQ (Fixture::activate (*m, kSerialTrial).code, juce::String ("bad_signature"));
    }
}

TEST (politica_1_acepta_backend_sin_firmar_pero_rechaza_firmas_falsas)
{
    Fixture f ("policy1");
    f.pub = testPublicKey();
    f.policy = 1;
    f.server.sign = false;
    { auto m = f.make(); CHECK (Fixture::activate (*m, kSerialFull).success); }      // backend viejo: OK
    f.licFile().deleteFile();
    f.server.sign = true; f.server.signNonceOverride = "otro";
    { auto m = f.make(); CHECK_EQ (Fixture::activate (*m, kSerialFull).code, juce::String ("bad_signature")); }
}

TEST (auditoria_con_firma_invalida_se_ignora_sin_bloquear)
{
    Fixture f ("audit_badsig");
    f.pub = testPublicKey();
    f.policy = 2;
    f.server.sign = true;
    auto m = f.make();
    CHECK (Fixture::activate (*m, kSerialFull).success);
    f.server.signNonceOverride = "repetida";
    f.clock += 5 * 3600 + 10;       m->runMaintenanceTickForTests();
    CHECK (m->isLicensed());                                          // un servidor falso no puede revocar ni renovar
}

//==============================================================================
int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::String filter = argc > 1 ? juce::String (argv[1]) : juce::String();

    int ran = 0;
    for (auto& t : registry())
    {
        if (filter.isNotEmpty() && ! juce::String (t.name).contains (filter)) continue;
        std::cout << "[ " << t.name << " ]\n";
        const int failedBefore = gFailed;
        t.fn();
        std::cout << (gFailed == failedBefore ? "    ok\n" : "    << CON FALLOS >>\n");
        ++ran;
    }

    std::cout << "\n" << ran << " pruebas, " << gChecks << " comprobaciones, " << gFailed << " fallos\n";
    return gFailed == 0 ? 0 : 1;
}
