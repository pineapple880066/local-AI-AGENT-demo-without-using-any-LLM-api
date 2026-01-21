// main.cpp 只是一个无关程序

// 这个文件“故意写错”：缺少 <thread> / <chrono> 两个 include， 

// 让它在第一次 ./build.sh 时编译失败，产生错误日志。
//
// 然后你用：
//   python3 -m agent.cli '修复示例项目编译错误' --workspace examples/demo_cpp
// agent 会自动补上缺失的 include，再次编译就能通过。
// chrono 时间库 -C11
#include <iostream>
#include <thread>
#include <chrono>

int main() {
  std::cout << "start\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::cout << "done\n";
  return 0;
}


