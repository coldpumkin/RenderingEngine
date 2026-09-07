# 두 경로를 한 번씩 그려서, 그림이 변했는지만 답한다.
#
# **왜 해시인가.** 렌더링이 맞는지는 눈으로 보면 "비슷해 보인다"까지밖에 안 나온다.
# 화면 921,600픽셀을 BMP로 쓰고 SHA-256으로 줄이면 64자리 숫자 하나가 되고, 그 숫자는
# 딱 하나만 답한다 - **변했나, 안 변했나.** 얼마나 변했는지는 안 알려준다.
#
# 쓸모는 그 하나로 충분하다. 죽은 값 두 줄을 지웠을 때 해시가 그대로면 그것이
# "저 값은 아무 데도 안 갔다"의 증명이고, 눈으로는 그 확신이 안 나온다.
#
# **전제: 같은 코드는 같은 그림을 내야 한다.** LAMBDA_CAPTURE가 t와 dt를 둘 다 고정하는
# 것이 그것이다 - 패널이 프레임 시간을 찍으므로 dt를 안 묶으면 기계 속도가 그림에
#들어가고, 그것만으로 두 실행의 해시가 갈렸다 (실측, 2026-09-06).
#
# **기준값이 여기 파일로 있는 이유.** 전에는 커밋 메시지에 손으로 적었고, 그래서 지금
# 값이 무엇인지 알려면 로그를 뒤져야 했다. 파일이면 git diff가 "이 커밋이 그림을
# 바꿨다"를 저절로 보여준다.
#
# 사용:
#   .\Tools\capture.ps1            두 경로를 돌려 기준값과 대본다
#   .\Tools\capture.ps1 -Accept    지금 값을 새 기준으로 적는다 (그림을 일부러 바꿨을 때)
#   .\Tools\capture.ps1 -Keep out  BMP를 그 폴더에 남긴다 (눈으로 볼 때)
#
# 빌드는 안 한다. 빌드한 뒤에 돌린다.

param(
    [switch]$Accept,
    [string]$Keep = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot          # Engine/
$exeDir = Join-Path $root "out\build\default\Source\Programs\sandbox"
$exe = Join-Path $exeDir "sandbox.exe"
$baselineFile = Join-Path $PSScriptRoot "capture.baseline"

if (-not (Test-Path $exe)) {
    Write-Host "sandbox.exe가 없다. 먼저 빌드한다: cmake --build --preset default" -ForegroundColor Red
    exit 1
}

# 경로 이름 -> 그 경로를 켜는 환경변수. 값이 $null이면 아무것도 안 켠다.
# 하나 더 늘 때 여기 한 줄이고, 아래는 손댈 것이 없다.
$paths = [ordered]@{
    "forward"  = $null
    "deferred" = "LAMBDA_DEFERRED"
}

$outDir = if ($Keep -ne "") { $Keep } else { Join-Path $env:TEMP "lambda-capture" }
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }

# 실행 디렉터리를 옮긴다. 셰이더를 "Shaders/..."로 상대 경로로 여니,
# 다른 곳에서 부르면 셰이더를 못 찾고 조용히 창만 뜬다.
Push-Location $exeDir

$now = [ordered]@{}
foreach ($name in $paths.Keys) {
    $bmp = Join-Path $outDir "$name.bmp"

    # 남의 오버레이 레이어는 우리 문제가 아니다. 실행할 때만 거른다.
    $env:VK_LOADER_LAYERS_DISABLE = "~implicit~"
    $env:LAMBDA_CAPTURE = $bmp
    $flag = $paths[$name]
    if ($flag -ne $null) { Set-Item -Path "env:$flag" -Value "1" }

    # **cmd 안에서 버린다.** PowerShell 5.1에서 네이티브 exe의 stderr를 `*>`나 `2>&1`로
    # 돌리면 줄마다 ErrorRecord로 감싸서 NativeCommandError가 되고, ErrorActionPreference
    # 가 Stop이라 거기서 죽는다. 종료 코드는 0이었는데도 그랬다.
    cmd /c "`"$exe`" >nul 2>&1"
    $code = $LASTEXITCODE

    # 켠 것은 끈다. 다음 경로가 물려받으면 두 줄이 같은 그림이 된다.
    if ($flag -ne $null) { Remove-Item -Path "env:$flag" -ErrorAction SilentlyContinue }
    Remove-Item -Path env:LAMBDA_CAPTURE -ErrorAction SilentlyContinue

    if ($code -ne 0 -or -not (Test-Path $bmp)) {
        Pop-Location
        Write-Host "$name : 캡처가 안 나왔다 (종료 코드 $code)" -ForegroundColor Red
        exit 1
    }
    $now[$name] = (Get-FileHash $bmp -Algorithm SHA256).Hash
}

Pop-Location

# ---- 기준값과 대보기 ----
$was = @{}
if (Test-Path $baselineFile) {
    foreach ($line in Get-Content $baselineFile) {
        $parts = $line -split '\s+', 2
        if ($parts.Count -eq 2) { $was[$parts[0]] = $parts[1].Trim() }
    }
}

$changed = $false
foreach ($name in $now.Keys) {
    $hash = $now[$name]
    if (-not $was.ContainsKey($name)) {
        Write-Host ("{0,-9} {1}  (기준 없음)" -f $name, $hash.Substring(0, 16)) -ForegroundColor Yellow
        $changed = $true
    } elseif ($was[$name] -eq $hash) {
        Write-Host ("{0,-9} {1}  같음" -f $name, $hash.Substring(0, 16)) -ForegroundColor Green
    } else {
        Write-Host ("{0,-9} {1}  바뀜  <- {2}" -f $name, $hash.Substring(0, 16),
                    $was[$name].Substring(0, 16)) -ForegroundColor Yellow
        $changed = $true
    }
}

if ($Accept) {
    $lines = foreach ($name in $now.Keys) { "$name $($now[$name])" }
    Set-Content -Path $baselineFile -Value $lines -Encoding utf8
    Write-Host "`n기준값을 새로 적었다: Tools/capture.baseline"
    Write-Host "**의도한 변화인지 확인하고 커밋한다.** git diff가 그 줄을 보여준다."
    exit 0
}

if ($Keep -ne "") { Write-Host "`nBMP: $outDir" }

# 바뀐 것이 있으면 0이 아닌 값. 스크립트에서 이어 붙일 수 있게.
if ($changed) {
    Write-Host "`n의도한 변화면 -Accept로 기준을 갱신한다."
    exit 2
}
exit 0
