# TypeScript 7 migration for melearner

Research date: 2026-07-10. Scope: `package.json`, `tsconfig.json`, `eslint.config.mjs`, `pnpm-lock.yaml`, and primary TypeScript, npm, and Next.js sources. No dependency or source changes were applied.

## Conclusion

Do not replace this repo's `typescript` dependency with TypeScript 7 directly. TypeScript 7.0 has no stable JavaScript programmatic API; the TypeScript team recommends running it beside the `@typescript/typescript6` compatibility package when tools import `typescript`, explicitly naming `typescript-eslint` as an example.[1] Next.js 16.2.10 requires `typescript/lib/typescript.js`, imports that API, and calls `createIncrementalProgram`, `createProgram`, and `getPreEmitDiagnostics` during `next build`.[5][6] The safe migration is therefore:

- keep TypeScript 6 exposed under the package name `typescript` for Next.js and ESLint;
- expose TypeScript 7's CLI through a second npm alias;
- run both compilers during the migration window;
- remove the two obsolete `tsconfig` settings described below.

The RC is no longer the best version for a new migration: npm now marks `7.0.2` as `latest`, while the RC is `7.0.1-rc`.[3] Use 7.0.2 for implementation; use the RC pin only when reproducing RC-specific behavior.

## Current repo state

- `typescript` is `^6.0.3`; `next` and `eslint-config-next` are `^16.2.10`; ESLint is `^9.39.4`.
- The lockfile resolves `eslint-config-next@16.2.10` to `typescript-eslint@8.62.1`. That package's TypeScript peer range is `>=4.8.4 <6.1.0`, so TypeScript 7 is outside its supported range.[4]
- `eslint.config.mjs` enables both `eslint-config-next/core-web-vitals` and `eslint-config-next/typescript`. No ESLint config change is needed if the package named `typescript` remains TypeScript 6.
- `tsconfig.json` uses supported migration-friendly settings such as `target: "ES2022"`, `module: "esnext"`, and `moduleResolution: "bundler"`. It has two settings to remove: `baseUrl` and `ignoreDeprecations`.
- The repo contains 53 `.ts`/`.tsx` files and no project-reference build graph.

## Exact recommended changes

Apply this dependency shape. The compatibility package is published as 6.0.2 but its `tsc6 --version` and exported API both report 6.0.3, so it preserves the repo's current effective TypeScript 6 version. Pin the wrapper package exactly because its package version and embedded compiler version differ.[1][3]

```diff
  "devDependencies": {
-   "typescript": "^6.0.3"
+   "typescript": "npm:@typescript/typescript6@6.0.2",
+   "typescript-7": "npm:typescript@7.0.2"
  }
```

Equivalent pnpm command:

```bash
pnpm add -D typescript@npm:@typescript/typescript6@6.0.2 typescript-7@npm:typescript@7.0.2
```

For an exact RC reproduction, change only `typescript-7` to `npm:typescript@7.0.1-rc`. Do not use floating `@rc` or `@latest` tags in the committed manifest.

Use separate, non-incremental checks so TypeScript 6 and 7 cannot overwrite the same `.tsbuildinfo` file:

```diff
  "scripts": {
-   "type-check": "tsc --noEmit",
+   "type-check": "tsc --noEmit --incremental false",
+   "type-check:ts6": "tsc6 --noEmit --incremental false --stableTypeOrdering",
-   "verify": "npm run type-check && npm run lint && npm run build && npm run tauri:check"
+   "verify": "npm run type-check:ts6 && npm run type-check && npm run lint && npm run build && npm run tauri:check"
  }
```

The TypeScript 6-only `stableTypeOrdering` flag makes its ordering match TypeScript 7 for migration diagnosis; TypeScript documents up to a 25% slowdown and says not to keep the flag as a long-term feature.[2] Keeping it on the comparison script, rather than in `tsconfig.json`, contains that cost.

Update `tsconfig.json` as follows:

```diff
-   "ignoreDeprecations": "6.0",
    "plugins": [
      { "name": "next" }
    ],
    "paths": {
      "@/*": ["./*"]
-   },
-   "baseUrl": "."
+   }
```

TypeScript 7 turns TypeScript 6 deprecations into hard errors and removes `baseUrl`; `paths` entries are resolved relative to the project root instead.[1][2] The existing `@/* -> ./*` mapping is already correct. Do not add `rootDir` or `types` preemptively: this root-level config already has the desired root, and the repo compiled under the RC's new defaults without an explicit `types` list.

## Compatibility risks

**Next.js 16.2.10:** direct TypeScript 7 installation is incompatible with Next's build-time checker. Its source requires `typescript/lib/typescript.js`, loads `require("typescript")`, and uses the TypeScript 6 JavaScript API.[5][6] Next only recognizes the older `@typescript/native-preview` package as a special case, and in that case it returns early rather than running build-time type checking.[5] Do not rely on that bypass, and do not set `typescript.ignoreBuildErrors`; Next warns that this skips its type-checking step and can be dangerous.[10] Keep the TypeScript 6 alias so `next build` retains its normal safety check. Next's documented `typescript.tsconfigPath` changes only the config file, not the compiler executable.[7]

**ESLint:** `eslint-config-next@16.2.10` depends on `typescript-eslint`, and the locked `typescript-eslint@8.62.1` supports TypeScript only below 6.1.[4][8] A direct TypeScript 7 replacement would be outside that peer range and would also remove the JavaScript API the parser expects. The TypeScript 6 alias keeps the existing flat config supported; no direct `typescript-eslint` dependency or ESLint config edit is recommended.

**TypeScript behavior:** the repo's `allowJs: true` means the TypeScript 7 JavaScript-analysis changes could matter if JavaScript files become included later. The current include patterns select `.ts` and `.tsx`, so this is not an immediate blocker. Unicode template-literal inference also changes for astral code points; no matching type-level code was identified in this migration inventory.[1]

**Runtime and package versions:** TypeScript 7 requires Node `>=16.20.0`, while Next.js 16.2.10 requires Node `>=20.9.0`; Next remains the effective floor.[3][9] The current local Node 24.15.0 satisfies both. The compatibility wrapper is package version 6.0.2 but currently exposes the 6.0.3 compiler/API, so validate both the package resolution and reported compiler version after every lockfile refresh.

## `--checkers` decision

Do not add `--checkers` to this repo's scripts initially. TypeScript 7 already defaults to four checker workers. The official guidance says more workers can help large codebases but consume more memory, while fewer workers can help constrained CI runners; changing the count can rarely expose order-dependent results.[1] With 53 TypeScript files, no project references, and a non-incremental check that already completes cleanly, an override has no demonstrated benefit. Benchmark `1`, `2`, and the default `4` only if CI time or memory becomes material, then pin one value consistently across environments. `--builders` is not applicable because this repo has no project-reference graph.

## Validation and rollback

1. Record the pre-migration baseline with `pnpm exec tsc --noEmit --incremental false --stableTypeOrdering`, `pnpm exec eslint .`, `pnpm build`, and `pnpm tauri:check`.
2. Make only the manifest, lockfile, script, and two `tsconfig` removals above in one migration commit. Run `pnpm install` and confirm the lockfile binds Next/`typescript-eslint` to `@typescript/typescript6@6.0.2`, not TypeScript 7.
3. Confirm `pnpm exec tsc --version` reports `7.0.2` (or `7.0.1-rc` for RC reproduction), `pnpm exec tsc6 --version` reports `6.0.3`, and `node -e "console.log(require('typescript').version)"` reports `6.0.3`.
4. Run `pnpm run type-check:ts6` and `pnpm run type-check`; treat any diagnostic difference as a migration failure until explained. Then run `pnpm run lint`, `pnpm run build`, and `pnpm run tauri:check`.
5. Repeat from a clean CI install with `pnpm install --frozen-lockfile`. Keep `next build` type checking enabled.
6. Roll back by reverting the single migration commit and reinstalling with the restored lockfile via `pnpm install --frozen-lockfile`; rerun the original `verify` pipeline. Do not retain the TypeScript 7 alias if either Next's TypeScript 6 check, ESLint, or TypeScript 6/7 diagnostic parity fails.

Research checks performed without changing project dependencies: TypeScript 6.0.3 type checking passed both normally and with `stableTypeOrdering`; ESLint passed; TypeScript 7.0.1-rc rejected the current config with `TS5102` for `baseUrl`; a temporary equivalent config without `baseUrl` completed under the RC with no diagnostics. An isolated pnpm install also verified the recommended aliases resolve `tsc` to 7.0.2 while both `tsc6` and `require("typescript")` report 6.0.3.

## Primary sources

1. TypeScript team, [Announcing TypeScript 7.0 RC](https://devblogs.microsoft.com/typescript/announcing-typescript-7-0-rc/).
2. TypeScript handbook, [TypeScript 6.0 release and migration notes](https://www.typescriptlang.org/docs/handbook/release-notes/typescript-6-0.html).
3. npm registry metadata: [`typescript@latest`](https://registry.npmjs.org/typescript/latest), [`typescript@7.0.1-rc`](https://registry.npmjs.org/typescript/7.0.1-rc), and [`@typescript/typescript6@latest`](https://registry.npmjs.org/@typescript%2ftypescript6/latest).
4. npm registry metadata, [`typescript-eslint@8.62.1`](https://registry.npmjs.org/typescript-eslint/8.62.1).
5. Next.js 16.2.10 source, [`verify-typescript-setup.ts`](https://github.com/vercel/next.js/blob/v16.2.10/packages/next/src/lib/verify-typescript-setup.ts).
6. Next.js 16.2.10 source, [`runTypeCheck.ts`](https://github.com/vercel/next.js/blob/v16.2.10/packages/next/src/lib/typescript/runTypeCheck.ts).
7. Next.js docs, [TypeScript configuration](https://nextjs.org/docs/app/api-reference/config/typescript).
8. Next.js 16.2.10 source, [`eslint-config-next/package.json`](https://github.com/vercel/next.js/blob/v16.2.10/packages/eslint-config-next/package.json).
9. npm registry metadata, [`next@16.2.10`](https://registry.npmjs.org/next/16.2.10).
10. Next.js docs, [`typescript.ignoreBuildErrors`](https://nextjs.org/docs/app/api-reference/config/next-config-js/typescript).
