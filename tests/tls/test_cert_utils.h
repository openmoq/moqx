#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <folly/testing/TestUtil.h>

namespace openmoq::o_rly::tls::test {

// Self-signed RSA cert+key pairs for testing, generated via:
//   openssl req -x509 -newkey rsa:2048 -nodes -days 3650
//               -subj '/CN=<name>' -keyout key.pem -out cert.pem
// NOLINTBEGIN(cert-*)

// CN=test.example.com
inline constexpr std::string_view kTestCertPem = R"(-----BEGIN CERTIFICATE-----
MIIDFzCCAf+gAwIBAgIUOnKhCz63sMKM5bJgAec46+CNL9EwDQYJKoZIhvcNAQEL
BQAwGzEZMBcGA1UEAwwQdGVzdC5leGFtcGxlLmNvbTAeFw0yNjAzMTAxMTMxMzFa
Fw0zNjAzMDcxMTMxMzFaMBsxGTAXBgNVBAMMEHRlc3QuZXhhbXBsZS5jb20wggEi
MA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCtY/D+jir6s7Dhm/j1c0BrBEKc
qAtgynmuL/ixx6PUNWXWvGmINWLFhBjiH/fMoN02Wp7DKzkq5sSwMzPypltG6gwc
G4MYpvarfV2fuB7qAXTk/HkhQ7XLZgLyNIoaPQSVFE45PlJJXpJgQnL7Z0bujkAB
GyXztaouks3toikmS24I17Ecb9iuNsHMjwrjxiKC07UTn3fISoZtTXDjSvir8JRP
4rM/+Ozhc1LUvtqgEYjpYsAAM2AX8hYdTCYNyPAdNzdYS0YTSDy/V4HbjOJq5tZd
TX06zet93/sylg6PR6i0soCMnTkRm+qQpQICfRGztGCMpWJ7h98gcXNKpvlHAgMB
AAGjUzBRMB0GA1UdDgQWBBSNjpgRXPw2ScI81af4PJCIZwQ0jDAfBgNVHSMEGDAW
gBSNjpgRXPw2ScI81af4PJCIZwQ0jDAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3
DQEBCwUAA4IBAQCIQKqF0YP+qlK/7CchMNDmW5GI0k2tJeict83QeItzXNrCn+fG
sYGqyHETYRODQjpArg/tDtt9EWwsy9JRX8ZGNcZlcL5PLaRPyBsn5qz5bjD/IiWo
hg0pv754jxZvuNd5SG4WmWaONoE8XsR3bW7NDlRtBGVviNMm8TFhC+3ryPT3f3fB
giG+p0UQzzHmpdygjtqTgxCzcNvYsbiVx/RPgdIWdbXJe1m2n0GCWyMVeCOquycm
detra9fkFX2K26KMTT7Qon8OYSnouwOsG/f7sQ40J0vo35WE1/C29DdKrJTvFszt
pkII1lrqJk10+t2RghWF2i57I7dzb+bJwHCl
-----END CERTIFICATE-----)";

inline constexpr std::string_view kTestKeyPem = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCtY/D+jir6s7Dh
m/j1c0BrBEKcqAtgynmuL/ixx6PUNWXWvGmINWLFhBjiH/fMoN02Wp7DKzkq5sSw
MzPypltG6gwcG4MYpvarfV2fuB7qAXTk/HkhQ7XLZgLyNIoaPQSVFE45PlJJXpJg
QnL7Z0bujkABGyXztaouks3toikmS24I17Ecb9iuNsHMjwrjxiKC07UTn3fISoZt
TXDjSvir8JRP4rM/+Ozhc1LUvtqgEYjpYsAAM2AX8hYdTCYNyPAdNzdYS0YTSDy/
V4HbjOJq5tZdTX06zet93/sylg6PR6i0soCMnTkRm+qQpQICfRGztGCMpWJ7h98g
cXNKpvlHAgMBAAECggEAH7p4mJQ6YCrulLI+cefXo12htNn5TwpuDsZfe2S9YXEu
BAfxRcgDHYKpLQPNjAfpwu79O1iW+vdEibus51uyuzzL337XU/UFkWb88WO3YHnI
wrhCkCg8RY6SvnCHzvpYctFG6Smy1BM2tN+j+8Yv0Cp+otUtcjXNgP1DKpdwcT2y
CfJ/xT+kINy+zzt3dZR8GZf2NMhJRJU8utf8Jar9RVWqVmehBl6+DhrUxbb1yom8
hn5nbkFbaJpTzhz5IRJvhTyQxVcLPLnnbmvPUv7LS/6qZpEftoHii2lKXejfHSD6
+JGWPbHqjuUm8gerjDF9cVPjw0JxrNM8PLDht9xFrQKBgQDhbehGQ35ugEM5sYqb
2KI+sqG13twhGF1t8IhqyWyye8oo4uyB6+llCbGr69D8Swq1TlBwV88GPfNkyGPd
jXRGH4IZAA4MG/M38r8TVOlg82WhiTiogL8nd1uyIT4dwbmrAPIzq+vCl257Oiq0
3Tv7koUSLKwv7z14cnKQGtkhAwKBgQDE524jI3+Iar/Ti4/QEuahu6MbRZLes0nD
/B6Kk0OOBf3jqMaaDh+Ni63osE1aHx1z5JzBsvEYxu1h7F3jNdnNH74w0n7a6g/n
0GwJPXhuvVdWS4luY1WwXpi9gnRXheyDEIBRVUkPvV9QmD6Pnzp0lPZJiWRpmyOf
n5264Qj5bQKBgCPOu3h9vBV9VjBR3TyIGq1u3nTvI3Q2VJDkBidAO33WX/RCp2Kz
wG0GLyyp1pZcrSTDfc96gy3wpTq7AfHtSCzjUFz8Pz75KZcXffZqJG/7+YbBLzjE
yphQQ0Z2NVGwtfdNvSssAdT1DN2SDbqQ8bgyO+T5J5itncwGEeCGAztVAoGAS1M5
d+nJjPdBYPz/zBqe7fopAHLSJ62wp2/Ygyyo6Dj0klXre92xRmXL5rsjLDnA+6fW
K+d3ggH/p7lThWsBYg4lpOmxq69k3EqIOdSxMLPwKEwHTBpmGm1lwwGX3i+WdeEn
JXYZ2BKa1usW67x/EUA3I5SSvC+kJhlarrYNx9UCgYEA1xabQ0ZBszTsxQz6oB+7
PrZYmVqlo3ylmLn+7ghGcig4l9ivRfjOFTSy/OTaukNiedh6aMwfz1GkhbikKai0
WXYWV/Iq7RsupsP+eGJx3tP6UkIXrkAFaXHrqHiwMfYR0naIAiBipObAOnpqFu7y
+P2OQBSxVVDDX+JiwJJcdp4=
-----END PRIVATE KEY-----)";

// CN=other.example.com
inline constexpr std::string_view kTestCert2Pem = R"(-----BEGIN CERTIFICATE-----
MIIDGTCCAgGgAwIBAgIUNXS0qZWo25EtkbwFYfswQnruGAswDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRb3RoZXIuZXhhbXBsZS5jb20wHhcNMjYwMzEwMTEzMTM1
WhcNMzYwMzA3MTEzMTM1WjAcMRowGAYDVQQDDBFvdGhlci5leGFtcGxlLmNvbTCC
ASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAOWcXNymcgccdEBC4e6fHued
HODQaV1Qi/vzOOAom4XfU5MpqETZgdtiz/3PkuaHLVzgswne0K+gUdIxnhKj8jhZ
x1FiOObu/z9a51NB2dFEvpX5WOpmJp06DkJBgQD2bIpCsg6qjDuooFN+NqW2fhQL
wBwe9zW8eLK+M/hwms4iWDbbrSSeAHmy3xpCmAlKAeguO20LAK2r6Wi0EKraI9vT
qSFMTKT+AMybrIWMKD1K3S+mZdgvaErxju9ssfJjQFmwsL4lgBf9yzz0+uHxjre5
aeOxFRgB+2Gyui7yFvxKgYVqxABeXNj1Afhul2RsSYE8u2+Qap6ZildgkADHDj0C
AwEAAaNTMFEwHQYDVR0OBBYEFE0IzNcgY6p5jMzqGSb/FbJkY8H1MB8GA1UdIwQY
MBaAFE0IzNcgY6p5jMzqGSb/FbJkY8H1MA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZI
hvcNAQELBQADggEBAOPEgvxu9I/8h2Lpv1RNNogNGbZSd7Cl9jizkbcanJ+meNFG
bHyvU+4zqPMjraXhO2JssK7T4IZzluIISG7BtyR8+kLm427fpbeUmt+fjCqbru1J
fyTAd855FGtgGn0NPLR5fCBmfiaV1ElNIdI87KK5MKKNuAqKbAByx1KaRAWn5rQd
y+D1/MNx1nCz35uMDWUP0MKx8H1XN1YpJOLj8tvPOcHBqrZ2F+U2Vg/5axKJnjbH
zbl3U0Ix5bA3qAe6s2Ifd6ZNq7I54ACM4tz+ZnWY6Rb7oiHErggnUXtQukyetevX
EWL806wbIE7SYnYdZPlBvF0n+etkB52mwPAvVPQ=
-----END CERTIFICATE-----)";

inline constexpr std::string_view kTestKey2Pem = R"(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDlnFzcpnIHHHRA
QuHunx7nnRzg0GldUIv78zjgKJuF31OTKahE2YHbYs/9z5Lmhy1c4LMJ3tCvoFHS
MZ4So/I4WcdRYjjm7v8/WudTQdnRRL6V+VjqZiadOg5CQYEA9myKQrIOqow7qKBT
fjaltn4UC8AcHvc1vHiyvjP4cJrOIlg2260kngB5st8aQpgJSgHoLjttCwCtq+lo
tBCq2iPb06khTEyk/gDMm6yFjCg9St0vpmXYL2hK8Y7vbLHyY0BZsLC+JYAX/cs8
9Prh8Y63uWnjsRUYAfthsrou8hb8SoGFasQAXlzY9QH4bpdkbEmBPLtvkGqemYpX
YJAAxw49AgMBAAECggEAAufRJq1y/fqtjGLTv3NjQc/kpBu3C3tdxIPYjsDxD8+j
Ctr6LXMg+A/aQnCD6paVWqdO7kpT6CXWbVtIadjOSwVKFdrGdHXKveuU2NWMtR+9
Bma38oIxT5lC7H82mq4Y8sDh8Ph4Wvvo95pPWfRP0RZ9LYmnYEmFHp4ODHiX6fer
uEFK46w69DOtWHtTE/2LH8k4ApwEiNO5ijRXaCUO4d25pFb7gECwFXe7bY7AtXnV
sI9vDfi4iSDgYLeOdS9fPy5hb3P6NI0g4i8KX1WENfKAhGix9jpftNWefB8SoZiY
B+kv5yFWjZv6I2sehe0Y67lclhsAsBDAobgL414+SQKBgQD6mPAys083frczM+HP
x+23m6ebV3lN3THSmvGObdtoVD/3rn8ohM3vEIyuBIcJPjnm4cyfHqLTT1YczwCk
da26FtUmxZQY03aB0nSxNfzHO/08fy51ToJyWdxw9KiNASizHSamQ7o6ngxFgTXZ
H5QXUaUh79w0LyteFZ4A5BuzNQKBgQDqj5kbTjA+EjuvD6ydELW1HHxmDXEo1m/X
Lzfk5N7OTPW+0K5kvRuJD7PxX1puolVPo9FyiJ6SMyGa0vI/QWFfF8LtIGZ1CrDG
IzkKbJyZWH/i11KobvmIlem/fSwkC+LUrWV0khDKtSaFUzyRhAggO/c3tNUgQW8J
v88kGdCH6QKBgESzjw5nSC1vqOv5qkubhRlULBQTXCczoAgcAGNKzN8CUfMmPKgw
GIEU6Wx/w0GOdLNObhmlfYAu/O2y9nsf4/vjbJZPjnVr685VkzZOFbnNQXTHbUYt
uud8qUmyWU8m5TCNql3krXaKg9S+QrP+y0vFT19JcfZAhEQr6wBViR6NAoGALzRI
5rLciJFYy4lG/rDvMIyUCGGqJULKbS7Ge90HbdMVHZqXjhR0pyeu2eOLqnom2wkn
zHnsF5YMrEDJmatJsj5w7xG3LNTC8I0EHLHw7fdefUNCEj2LIE6zJONG79Yohw6C
PWxrzq+YGfq/VLWSgRIwVViiD4S7mOWuBSDg04kCgYEAsR2QoLNJEp91o0bfm3wc
kyPBExIDppaIr5QNr+4xIFKv8jG5TtfK96fh6CE2PmK4tekcDXaisbsTkfZrYDIc
Y7d17T/IYJesXUQBKvz6POjn24BHOYRy9rAzqR7AixrS0stD6s8Gd3PaHtWnV6+i
LLG9cblFRFcOQE1olYuaAkY=
-----END PRIVATE KEY-----)";
// NOLINTEND(cert-*)

// RAII helper: writes cert/key files to a temp directory.
class TempCertDir {
public:
  TempCertDir() = default;
  TempCertDir(const TempCertDir&) = delete;
  TempCertDir& operator=(const TempCertDir&) = delete;

  void writeCert(const std::string& name, std::string_view certData, std::string_view keyData) {
    {
      std::ofstream ofs(dir_.path() / (name + ".crt"));
      ofs << certData;
    }
    {
      std::ofstream ofs(dir_.path() / (name + ".key"));
      ofs << keyData;
    }
  }

  void writeCertOnly(const std::string& name, std::string_view certData) {
    std::ofstream ofs(dir_.path() / (name + ".crt"));
    ofs << certData;
  }

  std::string path() const { return dir_.path().string(); }
  std::string filePath(const std::string& filename) const {
    return (dir_.path() / filename).string();
  }

private:
  folly::test::TemporaryDirectory dir_{"o_rly_tls_test"};
};

} // namespace openmoq::o_rly::tls::test
