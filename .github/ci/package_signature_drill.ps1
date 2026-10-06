# package_signature_drill.ps1 -- RED/GREEN controls for Test-PackageSignature.
#
# A check that has never been shown FAILING is not evidence of anything. Each arm builds a
# small zip around a fake main.dll of random bytes, signed by the signer's public TEST key,
# and requires Test-PackageSignature to either stay silent (the green arm) or to name the
# reason the arm is built to hit. A red that fails for another reason -- no python, a wrong
# path -- is a FAIL, so a broken drill cannot pass by accident.
#
# It lives apart from package_drill.ps1, which drills Test-PackageZip's tree rules only.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here 'ledger_lib.ps1')

$py = if (Get-Command python -ErrorAction SilentlyContinue) { 'python' }
      elseif (Get-Command python3 -ErrorAction SilentlyContinue) { 'python3' }
      else { throw 'package_signature_drill: neither python nor python3 is on PATH' }
$signer = Join-Path $here 'release_sign.py'

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('mv-pkg-sig-drill-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null

function New-FakeDll {
    param([string]$Path)
    $b = New-Object byte[] 256
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b)
    [System.IO.File]::WriteAllBytes($Path, $b)
}

# Sign a file with the public test key (never the release key), into $Out.
function New-TestSig {
    param([string]$Dll, [string]$Out)
    $line = & $py -I -B $signer sign --dll $Dll --target 0.9.0n --build 1 --out $Out --test-key
    if ($LASTEXITCODE -ne 0) { throw "package_signature_drill: the test-key sign failed: $line" }
}

$fails = 0
# $Build gets the stage folder, already holding mod/dlls/main.dll and its good .sig.
# $Reason is the text a violation must contain; '' means the arm expects a clean zip.
function SigArm {
    param([string]$Name, [scriptblock]$Build, [bool]$Trust, [string]$Reason, [string]$Because)
    $stage = Join-Path $tmp $Name
    New-Item -ItemType Directory -Path (Join-Path $stage 'mod/dlls') -Force | Out-Null
    $dll = Join-Path $stage 'mod/dlls/main.dll'
    New-FakeDll $dll
    New-TestSig $dll (Join-Path $stage 'mod/dlls/main.dll.sig')
    & $Build $stage
    $zip = Join-Path $tmp "$Name.zip"
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
    $v = @(if ($Trust) { Test-PackageSignature -ZipPath $zip -TrustTestKey } else { Test-PackageSignature -ZipPath $zip })
    $ok = if ($Reason) { @($v | Where-Object { $_ -like "*$Reason*" }).Count -gt 0 } else { $v.Count -eq 0 }
    if ($ok) {
        $verdict = if ($Reason) { 'RED  caught' } else { 'GREEN clean' }
        Write-Host ("  PASS  {0,-22} {1}" -f $Name, $verdict) -ForegroundColor Green
        if ($Reason) { $v | ForEach-Object { Write-Host "          -> $_" -ForegroundColor DarkGray } }
    } else {
        $script:fails++
        Write-Host ("  FAIL  {0,-22} expected '{1}', got {2} violation(s) -- {3}" -f $Name, $Reason, $v.Count, $Because) -ForegroundColor Red
        $v | ForEach-Object { Write-Host "          -> $_" -ForegroundColor DarkGray }
    }
}

Write-Host "package_signature_drill: Test-PackageSignature controls" -ForegroundColor Cyan

SigArm 'sig-green' { param($r) } $true '' 'a zip signed by the test key must verify when the test key is trusted'

SigArm 'sig-untrusted' { param($r) } $false 'verify failed at key' 'the test key is no release key and must not verify without -TrustTestKey'

SigArm 'sig-missing' { param($r)
    Remove-Item (Join-Path $r 'mod/dlls/main.dll.sig') -Force
} $true 'has no mod/dlls/main.dll.sig' 'a zip without the signature entry must fail'

SigArm 'sig-other-dll' { param($r)
    $other = Join-Path $r 'other.dll'
    New-FakeDll $other
    New-TestSig $other (Join-Path $r 'mod/dlls/main.dll.sig')
    Remove-Item $other -Force
} $true 'at sha' 'the signature of another DLL must not verify this one'

SigArm 'sig-flipped' { param($r)
    $sigPath = Join-Path $r 'mod/dlls/main.dll.sig'
    $t = [System.IO.File]::ReadAllText($sigPath)
    $i = $t.IndexOf("`nsig ") + 5
    $flip = if ($t[$i] -eq '0') { '1' } else { '0' }
    [System.IO.File]::WriteAllText($sigPath, $t.Substring(0, $i) + $flip + $t.Substring($i + 1), [System.Text.Encoding]::ASCII)
} $true 'at signature' 'one changed hex digit of the signature must fail'

Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ""
if ($fails -gt 0) { Write-Host "package_signature_drill: $fails ARM(S) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "package_signature_drill: ALL PASS" -ForegroundColor Green
