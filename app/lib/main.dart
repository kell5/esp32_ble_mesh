import 'package:flutter/cupertino.dart';

import 'pages/root_page.dart';

void main() {
  runApp(const MeshApp());
}

class MeshApp extends StatelessWidget {
  const MeshApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const CupertinoApp(
      title: '智能门铃',
      debugShowCheckedModeBanner: false,
      theme: CupertinoThemeData(primaryColor: CupertinoColors.activeBlue),
      home: RootPage(),
    );
  }
}
