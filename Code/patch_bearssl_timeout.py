                                                                           
 
                                                                  
                                                                         
                                                                              
                                           
                                                                              
                                                                                
                                                              
 
                                   
                                                                    
                                                                         
 
                                                                            
                                                                        
                                                                           
                                                               

Import("env")

from pathlib import Path
import atexit
import re

TIMEOUT_MARKER = "/* SAR_HARDEN3_PRESERVE_TLS_TIMEOUT */"
HOOK_DECL_MARKER = "/* SAR_HARDEN3_SERVICE_HOOK_DECL */"
HOOK_CALL_MARKER = "/* SAR_HARDEN3_SERVICE_HOOK_CALL */"
_restored = False
_target = None
_original = None

framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif8266")
if not framework_dir:
    raise RuntimeError("SmartAndRelax Harden3: ESP8266 Arduino framework package not found")

_target = Path(framework_dir) / "libraries" / "ESP8266WiFi" / "src" / "WiFiClientSecureBearSSL.cpp"
if not _target.exists():
    raise RuntimeError("SmartAndRelax Harden3: WiFiClientSecureBearSSL.cpp not found: %s" % _target)

raw = _target.read_text(encoding="utf-8")

                                                                          
                                                                           
raw = raw.replace(
    TIMEOUT_MARKER + "\n  // Keep the application-configured timeout across _freeSSL().",
    "_timeout = 15000;",
)
raw = raw.replace(
    HOOK_DECL_MARKER + '\nextern "C" void sar_bearssl_service_hook(void) __attribute__((weak));\n',
    "",
)
raw = raw.replace(
    "    " + HOOK_CALL_MARKER + "\n    if (sar_bearssl_service_hook) sar_bearssl_service_hook();\n",
    "",
)
_original = raw
patched = raw

                                                                              
                          
free_ssl_pattern = re.compile(
    r"(void\s+WiFiClientSecureCtx::_freeSSL\(\)\s*\{.*?"
    r"_handshake_done\s*=\s*false\s*;\s*)"
    r"_timeout\s*=\s*15000\s*;"
    r"(\s*\})",
    re.S,
)
patched, count_timeout = free_ssl_pattern.subn(
    lambda m: m.group(1) + TIMEOUT_MARKER + "\n  // Keep the application-configured timeout across _freeSSL()." + m.group(2),
    patched,
    count=1,
)
if count_timeout != 1:
    raise RuntimeError("SmartAndRelax Harden3: BearSSL _freeSSL() timeout pattern not found; build aborted safely")

                                                               
namespace_anchor = "namespace BearSSL {"
if patched.count(namespace_anchor) < 1:
    raise RuntimeError("SmartAndRelax Harden3: BearSSL namespace anchor not found; build aborted safely")
patched = patched.replace(
    namespace_anchor,
    HOOK_DECL_MARKER + '\nextern "C" void sar_bearssl_service_hook(void) __attribute__((weak));\n\n' + namespace_anchor,
    1,
)

                                                                             
                                                                   
run_until_pattern = re.compile(
    r"(int\s+WiFiClientSecureCtx::_run_until\(unsigned target, bool blocking\)\s*\{.*?"
    r"for\s*\(int no_work = 0; blocking \|\| no_work < 2;\)\s*\{\s*"
    r"optimistic_yield\(100\);)",
    re.S,
)
patched, count_hook = run_until_pattern.subn(
    lambda m: m.group(1) + "\n    " + HOOK_CALL_MARKER + "\n    if (sar_bearssl_service_hook) sar_bearssl_service_hook();",
    patched,
    count=1,
)
if count_hook != 1:
    raise RuntimeError("SmartAndRelax Harden3: BearSSL _run_until() hook pattern not found; build aborted safely")

_target.write_text(patched, encoding="utf-8")

env.Append(CPPDEFINES=[
    ("SAR_BEARSSL_TIMEOUT_PATCH", 1),
    ("SAR_BEARSSL_SERVICE_HOOK_PATCH", 1),
])


def _restore(*args, **kwargs):
    global _restored
    if _restored or _target is None or _original is None:
        return
    try:
        _target.write_text(_original, encoding="utf-8")
    finally:
        _restored = True


atexit.register(_restore)

print("SmartAndRelax Harden3: temporary BearSSL timeout + pump-service patches armed")
