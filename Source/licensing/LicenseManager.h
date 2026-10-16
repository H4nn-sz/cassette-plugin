#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <array>
#include <deque>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
//  CASSETTE · Licenciamiento
//
//  Reglas (ver README → "Licencias y anti-abuso"):
//   · C++ es la ÚNICA puerta de autorización. La UI (HTML) solo muestra estado y envía acciones.
//   · La ACTIVACIÓN siempre es online. Después, FULL funciona offline para siempre; PRUEBA/SUSCRIPCIÓN
//     funcionan offline hasta su fecha de expiración (absoluta, fijada por el servidor).
//   · Auditoría silenciosa cada 5 h contra /api/plugin/validate. Nunca bloquea audio ni UI.
//   · El registro local va sellado (HMAC) y atado al equipo: editarlo/copiarlo lo invalida.
//   · Todo el I/O de red y disco ocurre en un hilo propio. Nada de esto toca el hilo de audio.
// ─────────────────────────────────────────────────────────────────────────────
namespace CassetteLic
{

#ifndef CASSETTE_API_BASE
 #define CASSETTE_API_BASE "https://offszn.lat"
#endif

// 0 = no verificar firmas · 1 = rechazar firmas inválidas, aceptar respuestas sin firma (compatibilidad
// con un backend sin actualizar) · 2 = exigir firma válida siempre (recomendado tras desplegar el backend).
#ifndef CASSETTE_SIGNATURE_POLICY
 #define CASSETTE_SIGNATURE_POLICY 2
#endif

enum class LicenseKind { None, Trial, Full, Subscription };

enum class LockReason
{
    None,          // licencia válida
    NoLicense,     // nunca activada en este equipo
    Expired,       // prueba/suscripción vencida
    Revoked,       // el servidor la revocó / ya no es válida para este equipo
    Tampered,      // registro local alterado o copiado de otro equipo
    ClockTampered  // el reloj del sistema retrocedió
};

struct LicenseState
{
    bool         isValid    = false;
    bool         isTrial    = false;   // prueba o suscripción con fecha de fin (muestra cuenta atrás)
    LicenseKind  kind       = LicenseKind::None;
    LockReason   lockReason = LockReason::NoLicense;
    int          daysLeft   = 0;       // días enteros restantes (redondeo hacia arriba); 0 si no aplica
    int64_t      secondsLeft = 0;
    int64_t      expiresAt  = 0;       // unix (0 = sin vencimiento)
    int64_t      lastCheck  = 0;       // última auditoría online exitosa (unix)
    juce::String serial;               // enmascarado para mostrar en UI
    juce::String message;              // texto listo para mostrar
    juce::String code;                 // "none" | "trial" | "full" | "subscription" | "expired" | "revoked" | "tampered" | "clock"
};

struct ActivationResult
{
    bool         success = false;
    juce::String message;
    juce::String code;                 // offline | bad_response | not_found | revoked | expired | device_limit | trial_used | ...
};

struct HttpResponse
{
    bool         reached = false;      // false = no hubo respuesta HTTP (sin red, DNS, timeout, TLS)
    int          status  = 0;
    juce::String body;
};

// Todo lo "del mundo exterior" se inyecta aquí: en producción se usan los valores por defecto, los tests
// sustituyen reloj, red y disco para simular cada caso de abuso sin esperar días.
struct LicenseEnvironment
{
    juce::String apiBase;
    juce::File   primaryFile;                      // registro sellado principal
    bool         useMirror = true;                 // copia secundaria (registro de Windows / ~/.offszn)
    juce::String mirrorKey;                        // clave/archivo de la copia (tests la aíslan)
    std::function<int64_t()> nowFn;                // segundos unix
    std::function<HttpResponse (const juce::String& url, const juce::String& jsonBody, int timeoutMs)> httpPost;
    std::function<void (std::function<void()>)> dispatch;   // entrega callbacks (por defecto: hilo de mensajes)
    juce::String deviceId;                         // vacío = SystemStats::getUniqueDeviceID()
    juce::String computerName;                     // vacío = SystemStats::getComputerName()
    int          signaturePolicy = CASSETTE_SIGNATURE_POLICY;
    std::array<uint8_t, 32> publicKey {};          // clave pública Ed25519 del backend
    bool         startWorker = true;
    int64_t      checkIntervalSec = 5 * 3600;      // auditoría silenciosa
    int64_t      retryIntervalSec = 30 * 60;       // reintento tras fallo de red

    static LicenseEnvironment makeDefault();
};

class LicenseManager : private juce::Thread
{
public:
    LicenseManager();                              // producción (SharedResourcePointer)
    explicit LicenseManager (LicenseEnvironment env);
    ~LicenseManager() override;

    // ── Consulta (thread-safe, barata: seguro desde audio) ────────────────────
    bool         isLicensed() const noexcept       { return licensed.load (std::memory_order_relaxed); }
    uint32_t     getStateVersion() const noexcept  { return stateVersion.load(); }
    LicenseState getLicenseState() const;

    // ── Acciones (asíncronas; el callback llega por `dispatch`) ───────────────
    void activate (const juce::String& serial, std::function<void (ActivationResult)> callback);
    void startTrial (std::function<void (ActivationResult)> callback);
    void requestRevalidation();                    // fuerza una auditoría en cuanto haya red

    // ── Identidad del equipo ──────────────────────────────────────────────────
    juce::String getHardwareID() const;            // "<nombre>_<idUnico>" (mismo formato que Easy Mix / backend)
    static juce::String getDefaultHardwareID();

    // ── Solo tests ────────────────────────────────────────────────────────────
    void runMaintenanceTickForTests()              { maintenanceTick(); }
    void runPendingJobsForTests()                  { drainJobs(); }
    static juce::String buildV2Payload (const juce::String& serial, const juce::String& type, const juce::String& expiresStr,
                                        const juce::String& hwid, const juce::String& nonce, int64_t serverTime,
                                        const juce::String& status);

private:
    struct Record
    {
        juce::String serial;
        juce::String type;            // trial | lifetime | subscription
        int64_t expiresAt   = 0;
        int64_t activatedAt = 0;
        int64_t lastSeen    = 0;      // máximo reloj local observado
        int64_t serverFloor = 0;      // máxima hora de servidor observada
        int64_t lastCheck   = 0;
        juce::String status { "active" };   // active | revoked | expired
        bool valid = false;           // hay registro
    };

    struct Job
    {
        enum class Kind { Activate, Trial } kind = Kind::Activate;
        juce::String serial;
        std::function<void (ActivationResult)> callback;
    };

    struct Evaluation
    {
        bool valid = false;
        LockReason reason = LockReason::NoLicense;
        int64_t secondsLeft = 0;
        int daysLeft = 0;
    };

    // Hilo
    void run() override;
    void maintenanceTick();
    void drainJobs();

    // Red
    HttpResponse post (const juce::String& path, const juce::var& body);
    ActivationResult doActivate (const juce::String& serial);
    ActivationResult doStartTrial();
    void doAudit();
    bool verifyResponse (const juce::var& json, const juce::String& serial, const juce::String& hwid,
                         const juce::String& nonce, juce::String& outReason) const;
    static juce::String makeNonce();

    // Registro
    Record  loadBestRecord (bool& sawAnything, bool& allInvalid);
    bool    readRecordFrom (const juce::String& text, Record& out) const;
    juce::String serializeRecord (const Record& r) const;
    void    persist (const Record& r);
    juce::String computeMac (const juce::String& body) const;
    juce::String stableId() const;
    juce::String deviceTag() const;

    // Mirror
    juce::String readMirror() const;
    void writeMirror (const juce::String& text) const;

    // Estado
    Evaluation evaluate (const Record& r, bool sealOk, int64_t now) const;
    void publishState (const Record& r, const Evaluation& e, bool sealOk, bool sawAnything, int64_t now);
    juce::String maskSerial (const juce::String& s) const;
    int64_t now() const { return env.nowFn(); }

    LicenseEnvironment env;

    mutable juce::CriticalSection lock;
    Record        record;
    bool          sealOk        = true;
    bool          sawAnything   = false;
    int64_t       lastPersistedSeen = 0;
    juce::Time    knownFileTime;
    int64_t       nextAuditAttempt = 0;
    bool          revalidateRequested = false;
    LicenseState  cached;

    juce::CriticalSection jobLock;
    std::deque<Job> jobs;

    juce::CriticalSection httpLock;
    juce::WebInputStream* activeStream = nullptr;

    std::atomic<bool>     licensed { false };
    std::atomic<uint32_t> stateVersion { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LicenseManager)
};

} // namespace CassetteLic
