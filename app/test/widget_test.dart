import 'package:flutter_test/flutter_test.dart';

import 'package:mesh_app/config.dart';

void main() {
  test('doorbell topics derive from id', () {
    expect(AppConfig.doorbellEventTopic, 'doorbell/${AppConfig.doorbellId}/event');
    expect(AppConfig.doorbellCmdTopic, 'doorbell/${AppConfig.doorbellId}/cmd');
  });
}
