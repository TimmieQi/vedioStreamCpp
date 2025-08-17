# ==============================================================================
#  一键生成自签名证书及配套的 config.json 文件
#  用于 VideoStream QUIC 项目的快速开发环境搭建
# ==============================================================================

# --- 可配置变量 ---
$DnsName = "localhost" 
$ServerPort = 9998     
$ConfigFileName = "config.json"
# 【新增】控制是否为开发环境禁用Pacing
$PacingEnabledForDev = $false 

# --- 脚本开始 ---
Write-Host "--- 开始生成开发环境配置 ---" -ForegroundColor Green

# 1. 检查并移除旧的同名证书
Write-Host "1. 正在检查并清理旧证书..."
$OldCert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=$DnsName" }
if ($OldCert) {
    Write-Host "  找到旧证书，正在移除..." -ForegroundColor Yellow
    Remove-Item $OldCert.PSPath
} else {
    Write-Host "  未找到旧证书，继续。"
}

# 2. 创建一个新的自签名证书
Write-Host "2. 正在创建新的自签名证书 (CN=$DnsName)..."
$NewCert = New-SelfSignedCertificate -DnsName $DnsName -CertStoreLocation "Cert:\CurrentUser\My" -KeyUsage KeyEncipherment, DigitalSignature -NotAfter (Get-Date).AddYears(1)
if (-not $NewCert) {
    Write-Host "错误：创建证书失败！" -ForegroundColor Red
    exit 1
}

# 3. 提取证书的SHA-1指纹
$Fingerprint = $NewCert.Thumbprint
Write-Host "  成功创建证书，指纹(Thumbprint)为: $Fingerprint" -ForegroundColor Cyan

# 4. 创建配置对象
Write-Host "3. 正在生成 $ConfigFileName 文件..."
$ConfigObject = [PSCustomObject]@{
    server_port           = $ServerPort
    certificate_fingerprint = $Fingerprint
    pacing_enabled        = $PacingEnabledForDev # 【新增】
}

# 5. 将对象转换为JSON格式并保存到文件
$ConfigObject | ConvertTo-Json | Out-File -FilePath $ConfigFileName -Encoding utf8
Write-Host "  成功写入配置到 $ConfigFileName"

# --- 脚本结束 ---
Write-Host "--- 配置完成！ ---" -ForegroundColor Green