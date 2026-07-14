"""Render the PBR validation scene with the DXR shader's exact BRDF."""

from __future__ import annotations

import argparse
from pathlib import Path

import drjit as dr
import mitsuba as mi


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spp", type=int, default=512)
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).parents[1] / "Results/mitsuba_dxr_brdf_512spp.pfm")
    parser.add_argument("--variant", default="cuda_ad_rgb")
    return parser.parse_args()


def register_dxr_pbr_bsdf() -> None:
    class DxrPbrBSDF(mi.BSDF):
        def __init__(self, props):
            mi.BSDF.__init__(self, props)
            self.base_color = mi.Color3f(props["base_color"])
            self.metallic = float(props["metallic"])
            self.roughness = float(props["roughness"])
            flags = mi.BSDFFlags.GlossyReflection | mi.BSDFFlags.FrontSide
            self.m_components = [flags]
            self.m_flags = flags

        def distribution_ggx(self, n_dot_h):
            alpha = self.roughness * self.roughness
            alpha_squared = alpha * alpha
            n_dot_h_squared = n_dot_h * n_dot_h
            denominator = n_dot_h_squared * (alpha_squared - 1.0) + 1.0
            return alpha_squared / dr.maximum(
                dr.pi * denominator * denominator, 0.000001)

        def geometry_smith_height_correlated(self, n_dot_v, n_dot_l):
            alpha = self.roughness * self.roughness
            alpha_squared = alpha * alpha
            smith_v = n_dot_l * dr.sqrt(dr.maximum(
                n_dot_v * n_dot_v * (1.0 - alpha_squared) + alpha_squared, 0.0))
            smith_l = n_dot_v * dr.sqrt(dr.maximum(
                n_dot_l * n_dot_l * (1.0 - alpha_squared) + alpha_squared, 0.0))
            return (2.0 * n_dot_v * n_dot_l) / dr.maximum(
                smith_v + smith_l, 0.000001)

        @staticmethod
        def fresnel_schlick(cos_theta, f0):
            one_minus = 1.0 - dr.clip(cos_theta, 0.0, 1.0)
            one_minus_2 = one_minus * one_minus
            one_minus_5 = one_minus_2 * one_minus_2 * one_minus
            return f0 + (1.0 - f0) * one_minus_5

        @staticmethod
        def half_vector(wi, wo):
            raw = wi + wo
            inverse_length = dr.rsqrt(dr.maximum(dr.squared_norm(raw), 1e-20))
            return raw * inverse_length

        def evaluate_dxr(self, si, wo, active):
            wi = si.wi
            h = self.half_vector(wi, wo)
            n_dot_v = dr.maximum(dr.clip(wi.z, 0.0, 1.0), 0.0001)
            n_dot_l = dr.clip(wo.z, 0.0, 1.0)
            v_dot_h = dr.clip(dr.dot(wi, h), 0.0, 1.0)
            active &= (wi.z > 0.0) & (wo.z > 0.0)

            f0 = dr.lerp(mi.Color3f(0.04), self.base_color, self.metallic)
            d = self.distribution_ggx(dr.clip(h.z, 0.0, 1.0))
            g = self.geometry_smith_height_correlated(n_dot_v, n_dot_l)
            f = self.fresnel_schlick(v_dot_h, f0)
            specular = d * g * f / dr.maximum(4.0 * n_dot_v * n_dot_l, 0.0001)
            diffuse = ((1.0 - f) * (1.0 - self.metallic)
                       * self.base_color * dr.inv_pi)
            return dr.select(active, (diffuse + specular) * n_dot_l,
                             mi.Color3f(0.0))

        def pdf_dxr(self, si, wo, active):
            wi = si.wi
            h = self.half_vector(wi, wo)
            v_dot_h = dr.clip(dr.dot(wi, h), 0.0, 1.0)
            n_dot_h = dr.clip(h.z, 0.0, 1.0)
            active &= (wi.z > 0.0) & (wo.z > 0.0) & (v_dot_h > 0.0)
            pdf = self.distribution_ggx(n_dot_h) * n_dot_h / dr.maximum(
                4.0 * v_dot_h, 0.000001)
            return dr.select(active, dr.maximum(pdf, 0.000001), 0.0)

        def sample(self, ctx, si, sample1, sample2, active):
            del sample1
            alpha = self.roughness * self.roughness
            alpha_squared = alpha * alpha
            phi = sample2.x * (2.0 * dr.pi)
            cos_theta = dr.sqrt((1.0 - sample2.y) / dr.maximum(
                1.0 + (alpha_squared - 1.0) * sample2.y, 0.000001))
            sin_theta = dr.sqrt(dr.maximum(0.0, 1.0 - cos_theta * cos_theta))
            h = mi.Vector3f(sin_theta * dr.cos(phi),
                            sin_theta * dr.sin(phi), cos_theta)
            v_dot_h = dr.clip(dr.dot(si.wi, h), 0.0, 1.0)
            wo = 2.0 * v_dot_h * h - si.wi
            active &= (v_dot_h > 0.0) & (wo.z > 0.0) & (si.wi.z > 0.0)

            bs = mi.BSDFSample3f()
            bs.wo = wo
            bs.pdf = self.pdf_dxr(si, wo, active)
            bs.eta = 1.0
            bs.sampled_component = mi.UInt32(0)
            bs.sampled_type = mi.UInt32(+mi.BSDFFlags.GlossyReflection)
            value = self.evaluate_dxr(si, wo, active) / dr.maximum(bs.pdf, 1e-20)
            return bs, dr.select(active, value, mi.Color3f(0.0))

        def eval(self, ctx, si, wo, active):
            return self.evaluate_dxr(si, wo, active)

        def pdf(self, ctx, si, wo, active):
            return self.pdf_dxr(si, wo, active)

        def eval_pdf(self, ctx, si, wo, active):
            return self.evaluate_dxr(si, wo, active), self.pdf_dxr(si, wo, active)

        def to_string(self):
            return (f"DxrPbrBSDF[base_color={self.base_color}, metallic={self.metallic}, "
                    f"roughness={self.roughness}]")

    mi.register_bsdf("dxr_pbr", lambda props: DxrPbrBSDF(props))


def main() -> None:
    args = parse_args()
    mi.set_variant(args.variant)
    register_dxr_pbr_bsdf()
    scene_path = Path(__file__).with_name("pbr_scene.xml")
    scene = mi.load_file(str(scene_path), spp=args.spp)
    image = mi.render(scene, spp=args.spp)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    mi.Bitmap(image).write(str(args.output))
    print(f"Wrote {args.output} ({args.spp} spp, {args.variant})")


if __name__ == "__main__":
    main()
