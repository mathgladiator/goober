goober_package(
  NAME mail_consent
  SOURCES src/consent.cpp
  DEPS goober_sqlite goober_json goober_base
  TESTS test/consent_test.cpp
)
