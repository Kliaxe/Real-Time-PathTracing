#include <exception>
#include <iostream>

#include "Application.h"
#include "ApplicationOptions.h"

int main(int argc, char** argv)
{
  // Command line
  // Exit code 2 marks a usage error so scripts can tell a bad invocation apart from a rendering failure.
  // An error wins over --help so a malformed command line is never silently accepted.

  const rtpt::ApplicationOptionsParseResult parsed = rtpt::ParseApplicationOptions(argc, argv);

  if(!parsed.error.empty())
  {
    std::cerr << parsed.error << "\n\n" << rtpt::GetApplicationUsage(argv[0]);
    return 2;
  }

  if(parsed.showHelp)
  {
    std::cout << rtpt::GetApplicationUsage(argv[0]);
    return 0;
  }

  // Run
  // Initialization, rendering, and capture report failures by throwing. They are caught here and turned into exit code 1,
  // after the Application destructor has released every Vulkan and window resource.

  try
  {
    rtpt::Application application(parsed.options);
    return application.Run();
  }
  catch(const std::exception& error)
  {
    std::cerr << "Fatal error: " << error.what() << '\n';
    return 1;
  }
}
