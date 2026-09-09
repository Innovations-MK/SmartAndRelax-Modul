#pragma once
#include <Arduino.h>


bool cloudV2CredentialsLoad();
bool cloudV2CredentialsProvisioned();
String cloudV2CredentialsSecret();
bool cloudV2CredentialsSaveSecret(const String& secret);
bool cloudV2CredentialsClear();
