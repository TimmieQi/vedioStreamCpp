# ==============================================================================
#  一键生成自签名证书及配套的 config.json 文件
#  用于 VideoStream QUIC 项目的快速开发环境搭建
# ==============================================================================

# --- 可配置变量 ---
$DnsName = "localhost" # 证书绑定的主机名，用于本地开发
$ServerPort = 9998     # 服务器监听的端口
$ConfigFileName = "config.json" # 输出的配置文件名

# --- 脚本开始 ---
Write-Host "--- 开始生成开发环境配置 ---" -ForegroundColor Green

# 1. 检查并移除旧的同名证书，确保每次生成的都是最新的
Write-Host "1. 正在检查并清理旧证书..."
$OldCert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=$DnsName" }
if ($OldCert) {
    Write-Host "  找到旧证书，正在移除..." -ForegroundColor Yellow
    Remove-Item $OldCert.PSPath
} else {
    Write-Host "  未找到旧证书，继续。"
}

# 2. 创建一个新的自签名证书
#    - DnsName: 证书身份
#    - CertStoreLocation: 存储在当前用户的个人证书库中（MsQuic默认会在这里查找）
#    - KeyUsage: 确保证书可用于数字签名和密钥加密，这是TLS所必需的
#    - NotAfter: 设置证书有效期为1年
Write-Host "2. 正在创建新的自签名证书 (CN=$DnsName)..."
$NewCert = New-SelfSignedCertificate -DnsName $DnsName -CertStoreLocation "Cert:\CurrentUser\My" -KeyUsage KeyEncipherment, DigitalSignature -NotAfter (Get-Date).AddYears(1)

if (-not $NewCert) {
    Write-Host "错误：创建证书失败！请检查权限或PowerShell版本。" -ForegroundColor Red
    exit 1
}

# 3. 提取证书的SHA-1指纹 (Thumbprint)
#    MsQuic 需要的“证书哈希”就是这个 Thumbprint，它是一个十六进制字符串
$Fingerprint = $NewCert.Thumbprint
Write-Host "  成功创建证书，指纹(Thumbprint)为: $Fingerprint" -ForegroundColor Cyan

# 4. 创建一个PowerShell对象，用于构建JSON结构
Write-Host "3. 正在生成 $ConfigFileName 文件..."
$ConfigObject = [PSCustomObject]@{
    server_address        = $DnsName
    server_port           = $ServerPort
    certificate_fingerprint = $Fingerprint
}

# 5. 将对象转换为JSON格式并保存到文件
#    - ConvertTo-Json: 将PowerShell对象序列化为JSON字符串
#    - Out-File: 将字符串写入文件，使用UTF8编码
$ConfigObject | ConvertTo-Json | Out-File -FilePath $ConfigFileName -Encoding utf8

Write-Host "  成功写入配置到 $ConfigFileName"

# --- 脚本结束 ---
Write-Host "--- 配置完成！ ---" -ForegroundColor Green
Write-Host "现在您可以直接运行 VideoStreamServer.exe 和 VideoStreamClient.exe 了。"