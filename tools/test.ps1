$ErrorActionPreference = 'Stop'
$testOriginalPath = $env:PATH
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    # MSYS2 compiler subprocesses need the compiler's runtime DLL directory.
    $env:PATH = (Split-Path (Get-Command gcc).Source -Parent) + ';' + $env:PATH
    New-Item -ItemType Directory -Force .build | Out-Null
    gcc -std=c11 -Wall -Wextra -Werror -I mcx/source mcx/tests/test_protocol.c mcx/source/protocol.c -o .build/test_protocol.exe
    if ($LASTEXITCODE) { throw 'Protocol compilation failed' }
    & ./.build/test_protocol.exe
    if ($LASTEXITCODE) { throw 'Protocol tests failed' }
    gcc -std=c11 -Wall -Wextra -Werror -I mcx/source mcx/tests/test_encoder.c mcx/source/encoder.c -o .build/test_encoder.exe
    if ($LASTEXITCODE) { throw 'Encoder compilation failed' }
    & ./.build/test_encoder.exe
    if ($LASTEXITCODE) { throw 'Encoder tests failed' }
    gcc -std=c11 -Wall -Wextra -Werror -I mcx/source -c mcx/source/protocol.c -o .build/protocol.o
    if ($LASTEXITCODE) { throw 'Protocol object compilation failed' }
    g++ -std=c++11 -Wall -Wextra -Werror -I esp32/esp32_flight -I mcx/source esp32/tests/test_mcx_link.cpp .build/protocol.o -o .build/test_mcx_link.exe
    if ($LASTEXITCODE) { throw 'MCX link compilation failed' }
    & ./.build/test_mcx_link.exe
    if ($LASTEXITCODE) { throw 'MCX link tests failed' }
    g++ -std=c++11 -Wall -Wextra -Werror -I esp32/tests/mocks -I esp32/esp32_flight esp32/tests/test_tilt.cpp -o .build/test_tilt.exe
    if ($LASTEXITCODE) { throw 'Tilt compilation failed' }
    & ./.build/test_tilt.exe
    if ($LASTEXITCODE) { throw 'Tilt tests failed' }
    g++ -std=c++11 -Wall -Wextra -Werror -I esp32/esp32_flight esp32/tests/test_inputs.cpp -o .build/test_inputs.exe
    if ($LASTEXITCODE) { throw 'Input compilation failed' }
    & ./.build/test_inputs.exe
    if ($LASTEXITCODE) { throw 'Input tests failed' }
    node --test game/tests/input.test.js
    if ($LASTEXITCODE) { throw 'Browser tests failed' }
    python mcx/tools/esp32_sim.py --selftest
    if ($LASTEXITCODE) { throw 'Simulator tests failed' }
} finally {
    $env:PATH = $testOriginalPath
    Pop-Location
}
