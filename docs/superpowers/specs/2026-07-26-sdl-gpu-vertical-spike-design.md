# Phase 2 SDL_GPU vertical spike

Status: automatski odobren izvedbeni dizajn

Datum: 2026-07-26

Nadređeni contract:
`docs/superpowers/specs/2026-07-18-r3d-sustainable-renderer-fork-design.md`

## 1. Cilj

Phase 2 mora dokazati da jedan mali renderer iznad SDL_GPU-a može izvršiti
obvezni GPU podskup igre na eksplicitnom Vulkan backendu na Windowsu i
eksplicitnom Metal backendu na macOS-u.

Ovo je vertikalni spike, a ne novi defaultni game renderer. Postojeći legacy
bundle ostaje jedini shipping put dok spike ne prođe oba OS gatea. Spike živi
u `btbh-renderer` repozitoriju i ne uvodi SDL, Vulkan ili Metal tipove u
projektni Swift public seam.

## 2. Zaključane odluke

### 2.1 Dependency i ownership

- Renderer pinna SDL 3.4.2, jednaku verziju headera koju igra već koristi.
- SDL se gradi iz sourcea kao renderer dependency; ne oslanjamo se na slučajnu
  system instalaciju ni na relativni put do game repozitorija.
- Phase 2 kod je zaseban `btbh_gpu` C target i `btbh_gpu_spike` executable.
  Legacy `r3d` target ostaje nepromijenjen i paralelno izgradiv.
- Spike smije interno koristiti SDL tipove. Njegov exported header izlaže samo
  projektne enumove, trivijalne opise, opaque pokazivače i strukturirane
  rezultate.

### 2.2 Backend izbor

- Windows zahtijeva driver imena `vulkan`.
- macOS zahtijeva driver imena `metal`.
- Device se stvara kroz `SDL_CreateGPUDeviceWithProperties`.
- Windows oglašava samo SPIR-V i zahtijeva hardware-accelerated Vulkan.
- macOS oglašava samo `metallib`; MSL source nije production fallback.
- Nakon stvaranja `SDL_GetGPUDeviceDriver` mora vratiti točno traženi backend.
  D3D12, OpenGL, software Vulkan ili drugi fallback je hard failure.
- Validation build uključuje SDL GPU debug i verbose diagnostics.

### 2.3 Shader build

- Production/spike runtime ne prevodi shader source.
- Spike shader fixture koristi HLSL samo kao ulaz u pinned offline toolchain;
  to nije odluka da se svi budući game shaderi prebace na HLSL.
- Offline izlazi su SPIR-V za Vulkan te MSL + `metallib` za Metal.
- Svaki shader ima determinističan reflection manifest: stage, entry point,
  vertex inputs, sampler/storage/uniform brojevi, outputi i SHA-256 artefakta.
- Build pada ako manifest i C pipeline opis nisu podudarni.
- `metallib` se finalizira na macOS-u Apple alatima. Windows mora moći izgraditi
  i validirati SPIR-V te generirati cross-platform reflection contract.

### 2.4 Vertikalna scena

Jedna deterministička 1920x1080 scena dokazuje:

1. SDL window, high-DPI drawable, resize/minimize/restore i swapchain;
2. tri G-buffer color targeta i sampleable depth:
   - G0 `RGBA8_SRGB`: albedo + AO;
   - G1 `RGBA8_UNORM`: oct normal + roughness + metalness;
   - G2 `RGBA16_FLOAT`: emission + specular;
3. directional i spot shadow put;
4. minimalni deferred PBR + ambient fallback;
5. instanced opaque draw;
6. jedan skinned draw s bone podacima u storage bufferu;
7. jedan alpha-blended forward draw;
8. compute write u `RGBA16_FLOAT` storage teksturu pa graphics sample;
9. mali interval-trace compute kernel koji koristi budući RC binding obrazac;
10. readback finalne slike i capability/metrics JSON.

Scene seed, kamera, geometrija, materijali, svjetla i animation frame su fiksni.
Windows i Mac moraju prijaviti isti semantic scene signature.

## 3. Interni moduli

```text
btbh_gpu_spike
    |
    +-- platform/window fixture
    +-- deterministic scene fixture
    |
btbh_gpu public C seam
    |
    +-- device + capability report
    +-- resource registry + deferred release
    +-- command/frame lifecycle
    +-- readback + diagnostics
    |
SDL_GPU
    |
    +-- Vulkan (Windows)
    +-- Metal (macOS)
```

`btbh_gpu` ne preslikava cijeli SDL_GPU API. Dodaju se samo operacije koje
vertikalna scena stvarno koristi.

## 4. Lifecycle i greške

Redoslijed je:

1. inicijalizirati SDL video;
2. stvoriti high-DPI window;
3. stvoriti eksplicitni GPU device;
4. potvrditi driver i capability contract;
5. claimati window;
6. postaviti swapchain composition/present mode;
7. stvoriti persistent resurse i pipelineove;
8. izvršavati frameove s najviše tri frame slota;
9. završiti readback i fenceove;
10. otpustiti pipelineove/resurse;
11. releaseati window s devicea;
12. uništiti device, window i SDL.

Svaki neuspjeh daje stage, SDL poruku, traženi backend i djelomični capability
report. Dvostruki shutdown je siguran. Device loss u Phase 2 završava
kontroliranim izlazom i diagnostics dumpom.

## 5. Gateovi

Windows gate prolazi samo ako:

- driver je točno `vulkan`, hardware acceleration je zahtijevan;
- svi potrebni formati/usages i sample countovi su podržani;
- graphics, shadow, skinning, transparent i oba compute puta završe bez SDL
  validation greške;
- readback je 1920x1080 i scene signature je očekivan;
- dvije zagrijane serije daju stabilan screenshot i metrics;
- resize/high-DPI i clean shutdown probe prolaze.

Mac gate ponavlja isti contract s driverom `metal` i `metallib` shaderima.
Cross-backend SSIM cilj za stabilnu spike scenu je najmanje 0.98, uz ručni
pregled. Razlika u fontovima nije relevantna jer spike scena nema tekst.

Puni game performance target ostaje 16.67 ms p95. Spike dodatno bilježi CPU
submit i presented-frame median/p95, a GPU capture na svakom OS-u potvrđuje
passove i hitching. Broj iz jačeg lokalnog GPU-a nije zamjena za kasniji
GTX 1060/RX 580-class gate.

## 6. Stop uvjeti

SDL_GPU put se ne proglašava neuspješnim zbog prve implementacijske greške.
Alternativni backend audit pokreće se tek nakon reproducibilnog dokaza da:

- obvezni format/storage/compute feature nije dostupan na ciljnoj klasi;
- obvezni game feature nije moguće izraziti bez curenja backend API-ja;
- ili optimizirani spike i GPU capture pokazuju neprihvatljivu regresiju.

Tada se ista scena, shader contract i mjerenja koriste za bgfx, direktni
Vulkan + Metal ili Vulkan + MoltenVK usporedbu.

## 7. Ne-ciljevi

- uključivanje GPU backenda kao defaulta igre;
- puni asset importer ili svih pet launch modova;
- puni Canvas2D i font renderer;
- SSAO, SMAA, LUT, CAS i tilt-shift paritet;
- production Radiance Cascades;
- CameraProfile;
- uklanjanje legacy r3d/raylib/OpenGL koda;
- D3D12 fallback.
