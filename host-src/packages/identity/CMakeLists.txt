goober_package(
  NAME identity
  SOURCES src/identity.cpp src/oauth.cpp src/invites.cpp src/deletion.cpp
  DEPS goober_base goober_json goober_sqlite
  TESTS test/identity_test.cpp test/oauth_test.cpp test/invites_test.cpp test/deletion_test.cpp
)
