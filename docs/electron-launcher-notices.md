# Electron launcher license inventory

Status: preliminary inventory for review, not clearance to publish. The development ZIP sent for private testing is not a release. Before any distribution, reconcile every shipped component and transitive binary dependency with `THIRD_PARTY_NOTICES.md`, include the applicable license and notice texts, and extend `tests/shipped_notices_check.py` for the final package. Recheck the exact artifacts and versions at release time.

## Runtime components

- **Electron 45.4.1:** MIT license. The Electron binary distribution contains `LICENSE`, the Electron copyright and MIT license text. Source: [Electron LICENSE](https://github.com/electron/electron/blob/v45.4.1/LICENSE). Electron combines Chromium and Node; its own MIT notice is not a substitute for their notices.
- **Chromium:** primarily BSD 3-Clause, with many bundled third-party components under their own licenses. The tested Electron binary reports Chromium 156.0.8078.100 and includes `LICENSES.chromium.html`, a collection of third-party notices. Source: [Chromium license](https://chromium.googlesource.com/chromium/src/+/main/LICENSE). Review the exact bundled notice file and verify its inclusion in each platform artifact; do not treat a single BSD row as an exhaustive inventory of all bundled components.
- **Node.js:** MIT license, plus notices for its own bundled components. The tested Electron binary reports Node 24.22.3; its distributed `LICENSES.chromium.html` contains a Node.js section. Sources: [Node.js LICENSE](https://github.com/nodejs/node/blob/main/LICENSE) and the license file included in the exact Electron binary. Verify that the complete applicable Node and bundled dependency notices are present in each final distribution, not just the top-level MIT text.
- **Python embedded runtime:** the private Windows test ZIP used the official Python 3.11.9 x64 embeddable distribution, with the PSF License Version 2 and additional applicable licenses. It includes `resources/python/LICENSE.txt`. Sources: [Python 3.11.9 downloads](https://www.python.org/downloads/release/python-3119/) and [Python license and acknowledgements](https://docs.python.org/3.11/license.html). The replacement ZIP contains worker bytecode and uses the Python 3.14 distribution already present in the selected installation. The runtime and its notices remain in that installation.
- **Runtime npm packages:** the current application declares no production npm dependencies; the replacement test ZIP contains compiled application JavaScript, worker bytecode, and the Electron distribution. It contains no `node_modules` directory. Audit the final package and its complete dependency tree before release.

## Development and test only

TypeScript, `@types/node` and `playwright-core` are build or test dependencies and are not included in the private runtime ZIP. Electron's npm installer and its transitive packages are used to obtain/build the development environment; the final artifact must be checked to ensure they do not inadvertently ship. Their package licenses and the lockfile remain relevant for development; add a runtime notice if packaging changes to include any of them. No new dependency is proposed by this inventory.

## Remaining release work

1. Select and pin each actual platform runtime and enumerate its shipped components, including embedded/native dependencies and their notices.
2. Add the required entries to `THIRD_PARTY_NOTICES.md` and ship the correct complete texts in the final artifact. Verify the project-specific license and notices as well.
3. Extend the packaging and `shipped_notices_check` coverage to check the Electron artifacts, and inspect the resulting Windows and Linux packages. Preserve the Chromium/Node and Python notice stacks rather than replacing them with short labels.
4. Review the privately distributed test ZIP's omissions before any wider redistribution. The inventory above does not establish that it meets every distribution requirement.
