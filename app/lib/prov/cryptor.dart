import 'dart:typed_data';

import 'package:pointycastle/export.dart';

/// Pure-Dart replacement for the original plugin's native MethodChannel cipher.
///
/// ESP protocomm Security1 uses AES-256-CTR as a keystream that advances across
/// every message in the session (the counter keeps incrementing), so a single
/// stateful cipher instance is kept alive between [crypt] calls. CTR is
/// symmetric, hence encrypt and decrypt are the same operation.
class Cryptor {
  CTRStreamCipher? _cipher;

  Future<bool> init(Uint8List key, Uint8List iv) async {
    _cipher = CTRStreamCipher(AESEngine())
      ..init(true, ParametersWithIV<KeyParameter>(KeyParameter(key), iv));
    return true;
  }

  Future<Uint8List> crypt(Uint8List data) async {
    final cipher = _cipher;
    if (cipher == null) {
      throw StateError('Cryptor.init must be called before crypt');
    }
    return cipher.process(data);
  }
}
