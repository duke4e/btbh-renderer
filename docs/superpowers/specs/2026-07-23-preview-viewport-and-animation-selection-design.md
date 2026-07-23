# Level-up 3D preview viewport i stabilan odabir animacija

Status: koncept odobren; pisani dizajn čeka završni pregled

Datum: 2026-07-23

Opseg: BTBHRenderer/r3d fork i legacy adapter igre

## 1. Problem

Level-up i player-type 3D preview trenutačno renderiraju punu R3D scenu u default framebuffer, a zatim Canvas2D scissorom pokušavaju zadržati samo preview pravokutnik. Scene backend radi u drawable pikselima, dok preview layout i Canvas2D naredbe nastaju u logičkim UI koordinatama. Taj skriveni cross-backend state na high-DPI prozoru ostavlja bijeli rub veličine UI `outerPaddinga`.

Model animacije imaju odvojen kvar. Game catalog ispravno koristi zero-based indekse; player GLB ima 25 clipova i, primjerice, `idle` je 17, a `walk` 24. R3D importer ipak sprema samo 31 znak naziva. Svih 25 dugih Mechagodzilla naziva zato kolidira kao `Mechagodzilla_Legendary_skeleto`. Legacy adapter validira indeks, pretvara ga natrag u skraćeni naziv, a animation tree ponovno traži clip po tom nazivu i dobiva prvi match. Indeks se ne pomiče; njegov identitet se izgubi u round-tripu kroz naziv.

## 2. Odluka

Preview postaje pravi scene viewport i više ne koristi Canvas2D scissor. Public renderer seam dobiva normalizirani top-left viewport u `FrameDescriptoru`. Backend ga jednom pretvara u drawable piksele i predaje kao `R3D_View.viewport`. R3D završni blit tada pokriva samo traženi pravokutnik.

Animation tree dobiva eksplicitan by-index creation API. Legacy adapter nakon validacije predaje originalni zero-based indeks izravno R3D-u; više nema index → truncated name → index round-tripa. Importer istodobno prestaje rezati animation nazive i čuva puni Assimp naziv radi dijagnostike i budućeg name-based odabira.

## 3. Preview arhitektura

### 3.1 Public seam

BTBHRenderer uvodi vlastiti `NormalizedViewport`:

- `x`, `y`, `width` i `height` su konačni `Float`;
- koordinatni početak je gore lijevo;
- sva četiri ruba moraju biti unutar zatvorenog raspona `0...1`;
- širina i visina moraju biti veće od nule;
- `.full` je `(0, 0, 1, 1)`.

`FrameDescriptor` dobiva `viewport: NormalizedViewport = .full`. Default zadržava ponašanje svih gameplay i editor frameova bez promjene call siteova.

Normalizirani ugovor je namjeran: game layout poznaje logičku veličinu, backend poznaje drawable veličinu, a nijedna strana ne mora pogađati Retina scale faktor.

### 3.2 Pixel conversion

Level-up preview pretvara svoj logički `RenderRect` u normalizirane rubove dijeljenjem s aktualnom logičkom širinom i visinom. Legacy backend pretvara normalizirane rubove u drawable piksele:

1. zasebno zaokruži lijevi, gornji, desni i donji rub;
2. izračuna širinu kao `right - left`, a visinu kao `bottom - top`;
3. odbije prazan ili izvan-bounds rezultat;
4. iz veličine tog viewporta izračuna internu R3D rezoluciju uz `renderScale`;
5. preda rezultat R3D-u u top-left koordinatama koje `R3D_View` već definira.

Zaokruživanje rubova, umjesto zasebnog skaliranja širine, sprječava jednopoljne pukotine između previewa i Canvas2D obruba.
Interni render target koristi veličinu preview viewporta, a ne cijelog prozora, pa popravak ne uvodi full-frame GPU trošak za mali preview.

### 3.3 Frame flow

Novi preview frame ide ovim redoslijedom:

1. Canvas2D flush završi sve prethodne UI naredbe.
2. Scene backend započne frame s preview kamerom i normaliziranim viewportom.
3. Preview preda modele, ground, čestice i projektile.
4. R3D renderira i prezentira isključivo u izračunati drawable viewport.
5. Scene frame završi bez aktivacije globalnog scissora.
6. Canvas2D nastavi s overlayem, obrubom, karticama i tekstom.

`QueuedSceneFrameLifecycle` više nema `activateClip` i `releaseClip` faze za ovaj put. Opći Canvas2D scissor ostaje podržan za stvarne 2D potrebe.

### 3.4 Kamera

Preview camera controller već prati preview playera na world originu. Kad R3D projekcija koristi aspect samog viewporta, player prirodno završava u centru lokalnog NDC prostora. Briše se `centeredPreviewCamera` korekcija koja trenutačno pokušava premjestiti full-screen projekciju prema apsolutnom UI centru.

Ovo zadržava postojeći camera mutator i buduće player-type kutove: oni mijenjaju poziciju/orijentaciju kamere, ne presentation viewport.

## 4. Animation identitet

### 4.1 Puni nazivi

`R3D_Animation.name` prelazi s fiksnog `char[32]` buffera na library-owned C string:

- importer alocira točnu duljinu Assimp naziva i nul terminator;
- animation library posjeduje string;
- `R3D_UnloadAnimationLib` oslobađa svaki naziv;
- neuspjela alokacija uredno poništava učitavanje tog clipa bez curenja;
- `R3D_GetAnimationIndex` i `R3D_GetAnimation` uspoređuju puni naziv.

Ovo je namjerna ABI promjena forka. Native biblioteku i game adapter treba graditi zajedno; stari `libr3d.a` nije kompatibilan s novim headerima.

`R3D_AnimationNodeParams.name` istodobno postaje `const char*`. Name-based creation nakon lookup-a u node sprema canonical library-owned `animation->name`, a ne callerov privremeni pointer. Time postojeći name-based API stvarno može primiti i vratiti puni naziv bez novog lifetime zahtjeva za callera.

### 4.2 Animation tree API

R3D dodaje:

```c
R3D_AnimationTreeNode* R3D_CreateAnimationNodeByIndex(
    R3D_AnimationTree* tree,
    int animationIndex,
    R3D_AnimationNodeParams params
);
```

Funkcija:

- validira `tree`, library i zero-based indeks;
- uzima `&tree->player.animLib.animations[animationIndex]` izravno;
- veže taj pointer uz leaf node;
- koristi isti state, loop, callback i root-motion setup kao postojeći name-based API;
- vraća `NULL` i precizan warning za nevaljani indeks.

Postojeći `R3D_CreateAnimationNode` ostaje radi kompatibilnosti i traži po punom nazivu. Interno oba javna entry pointa pozivaju jednu funkciju koja prima već razriješen `R3D_Animation*`, kako se njihove semantike ne bi razišle.

Legacy adapter koristi isključivo `R3D_CreateAnimationNodeByIndex` za `AnimationClipDescriptor.animationIndex`. Game catalog ostaje zero-based i ne dobiva globalni `+1` ili `-1`.

## 5. Greške i lifecycle

- Nevaljan normalizirani viewport zaustavlja `beginFrame` strukturiranom adapter greškom; nema clampanja koje bi prikrilo caller bug.
- Nevaljan animation indeks zaustavlja stvaranje game animation treeja s nazivom logičkog clipa i problematičnim indeksom.
- Animation library mora nadživjeti animation player i tree, kao i danas.
- Preview scene frame mora zatvoriti viewport state prije nastavka Canvas2D naredbi.
- R3D mora vratiti prethodni OpenGL viewport nakon `R3D_End`; preview više ne ovisi o stanju scissor testa.

## 6. Testovi i acceptance kriteriji

### 6.1 Renderer testovi

- `.full` viewport daje cijeli drawable.
- Preview viewport daje iste pikselne rubove na 1280×720 i proporcionalnom 2048×1152 drawableu.
- Rubno zaokruživanje nema gap ni overlap.
- Nevaljani, NaN i prazni viewporti se odbijaju.
- Fixture s dva animation naziva koji dijele prvih 31 znak zadržava oba puna i različita naziva.
- Name lookup nalazi oba puna naziva.
- By-index node 0 i node 1 vežu različite `R3D_Animation` objekte.
- Out-of-range indeks vraća `NULL`.

### 6.2 Game adapter testovi

- Default gameplay descriptor ostaje `.full`.
- Preview descriptor nosi normalizirani card viewport.
- Preview lifecycle više ne šalje Canvas `pushScissor`/`popScissor`.
- Animation tree prosljeđuje catalog indeks direktno u by-index R3D API.
- Player `idle=17` i `walk=24` ostaju nepromijenjeni.

### 6.3 Integracija

- Čisti renderer build i test suite prolaze.
- Novi `libr3d.a` se kopira u game paket i provjeri mu se target architecture.
- Čisti SwiftPM game build i `Main` Xcode scheme prolaze.
- Player-type preview nema bijeli lijevi ni donji rub na macOS high-DPI prozoru.
- Preview sadržaj dodiruje sva četiri unutarnja ruba UI okvira bez prepisivanja okvira.
- Player i reprezentativni enemy modeli reproduciraju namjeravane idle/walk/attack clipove.
- Legacy render baseline diff nema regresiju izvan namjerno promijenjenog preview područja.

## 7. Izvan opsega

- Novi Metal/Vulkan/SDL_GPU backend.
- Offscreen preview render target; ostaje fallback samo ako direktni viewport ne može zadovoljiti acceptance kriterije.
- Promjena game cataloga s indeksa na semantičke nazive.
- Redizajn camera mutatora ili player-type camera profila.
- Globalno uklanjanje Canvas2D scissora.
