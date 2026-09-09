# Native image publication checks

The root Dockerfile runs `docker/run-native-publication-tests.sh` in its builder
before assembling the runtime image. Both the amd64 and arm64 GitHub publishing
jobs use this Dockerfile and portable build settings, so a failed native test
prevents that architecture from being pushed and blocks rolling-tag promotion.
There is no Docker build argument to disable this gate.

The gate builds only the named native test targets and runs:

- Strict parent metadata decoding and consensus metadata extraction, with the
  metadata startup selector explicitly set to 0 and 1 in separate processes.
- Local overlay signature receipts and prepared keyring signing.
- Admission batches, refresh policy/integration, external-message scheduling,
  callback wait attribution, routing, execution scratch and state-resolver policy.
- `test-cells --filter NativeStateEngine` and native load-generator policy.

Other `test-cells`/TVM tests and the general CTest suite are not invoked. Each
executable has a 300-second deadline, followed by forced termination after ten
seconds if necessary. The additional target build has a one-hour deadline.
Every TD test invocation must report at least one started and passed test, with
matching counts and a final success summary; exit zero from an empty filter is
a failure. The standalone state-resolver policy executable has unconditional
assertion checks and no test-selection mechanism. Any command failure stops the
gate. Logs remain in the builder's `build/native-publication-logs` directory and
failed logs are printed to the build output; test binaries are not copied into
the published runtime image.

For an already configured local build:

```sh
bash docker/run-native-publication-tests.sh --build-dir build --jobs 2
```

`--run-only` checks existing local binaries without rebuilding and therefore
does not establish source freshness. Docker always uses the build-and-run route.
The standalone gate regression checks need no compiler or Docker:

```sh
python3 docker/tests/test_native_publication_gate.py
```

Image identity checks remain separate: require successful publishing jobs for
the intended source revision, verify its architecture manifests and pulled
revision/binary version, and freeze the resolved digest for production tests.
These correctness checks establish neither a TPS improvement nor maximum
capacity. Reusing a successful Docker build layer reuses checks for that exact
cached builder filesystem and instruction; it cannot select tests from a
different source tree.
