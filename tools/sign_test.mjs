// Firmante de PRUEBA para los tests de C++ (GhostLicenseTests). Usa una clave Ed25519 determinista y pública que
// NO tiene relación con la clave de producción (PLUGIN_PRIVATE_KEY).
//   node sign_test.mjs pub            -> clave pública (64 hex)
//   node sign_test.mjs sign <payload> -> firma (128 hex)
import crypto from 'crypto';

const seed = Buffer.alloc(32, 7); // semilla fija de pruebas
const pkcs8 = Buffer.concat([Buffer.from('302e020100300506032b657004220420', 'hex'), seed]);
const priv = crypto.createPrivateKey({ key: pkcs8, format: 'der', type: 'pkcs8' });

const [cmd, ...rest] = process.argv.slice(2);
if (cmd === 'pub') {
  const spki = crypto.createPublicKey(priv).export({ format: 'der', type: 'spki' });
  process.stdout.write(spki.subarray(spki.length - 32).toString('hex'));
} else if (cmd === 'sign') {
  process.stdout.write(crypto.sign(null, Buffer.from(rest.join(' '), 'utf8'), priv).toString('hex'));
} else {
  process.stderr.write('uso: pub | sign <payload>\n');
  process.exit(2);
}
