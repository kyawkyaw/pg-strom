/*
 * opencl_textlib.h
 *
 * Collection of text functions for OpenCL devices
 * --
 * Copyright 2011-2014 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014 (C) The PG-Strom Development Team
 *
 * This software is an extension of PostgreSQL; You can use, copy,
 * modify or distribute it under the terms of 'LICENSE' included
 * within this package.
 */
#ifndef OPENCL_TEXTLIB_H
#define OPENCL_TEXTLIB_H
#ifdef OPENCL_DEVICE_CODE


#if 1
static int
varlena_cmp(__private cl_int *errcode,
			__global varlena *arg1, __global varlena *arg2)
{
	__global cl_char *s1 = VARDATA_ANY(arg1);
	__global cl_char *s2 = VARDATA_ANY(arg2);
	cl_int		len1 = VARSIZE_ANY_EXHDR(arg1);
	cl_int		len2 = VARSIZE_ANY_EXHDR(arg2);
	cl_int		len = min(len1, len2);

	/*
	 * XXX - to be revised for more GPU/MIC confortable coding style.
	 * Once thing you need to pay attention is varlena variables may
	 * be unaligned if short format, thus it leads unaligned data
	 * access, then eventually leads kernel crash.
	 */
	while (len > 0)
	{
		if (*s1 < *s2)
			return -1;
		if (*s1 > *s2)
			return 1;

		s1++;
		s2++;
		len--;
	}
	if (len1 != len2)
		return (len1 > len2 ? 1 : -1);
	return 0;
}
#else
/*
 * NOTE: optimal version of varlena_cmp, however, we could not observe
 * performance benefit on random text strings. It may work effectively
 * towards the string set with duplicated values, but not tested.
 */
static int
varlena_cmp(__private cl_int *errcode,
			__global varlena *arg1, __global varlena *arg2)
{
	__global cl_char *s1 = VARDATA_ANY(arg1);
	__global cl_char *s2 = VARDATA_ANY(arg2);
	cl_int		len1 = VARSIZE_ANY_EXHDR(arg1);
	cl_int		len2 = VARSIZE_ANY_EXHDR(arg2);

	cl_int		alignMask	= sizeof(cl_int) - 1;

	cl_ulong	buf1		= 0;
	cl_ulong	buf2		= 0;
	cl_int		bufCnt1		= 0;
	cl_int		bufCnt2		= 0;

	cl_int				addr1, firstChars1, intLen1, rest1;
	cl_int				addr2, firstChars2, intLen2, rest2;
	__global cl_uchar *	src1c;
	__global cl_uchar *	src2c;
	__global cl_uint *	src1i;
	__global cl_uint *	src2i;
	int					i, j;

	addr1		= (size_t)s1;
	firstChars1	= ((sizeof(cl_uint) - (addr1 & alignMask)) & alignMask);
	firstChars1	= min(firstChars1, len1);
	intLen1		= (len1 - firstChars1) / sizeof(cl_uint);
	rest1		= (len1 - firstChars1) & alignMask;

	addr2		= (size_t)s2;
	firstChars2	= ((sizeof(cl_uint) - (addr2 & alignMask)) & alignMask);
	firstChars2	= min(firstChars2, len2);
	intLen2		= (len2 - firstChars2) / sizeof(cl_uint);
	rest2		= (len2 - firstChars2) & alignMask;

	/* load the first 0-3 characters */
	src1c = (__global cl_uchar *)s1;
	for(i=0; i<firstChars1; i++) {
		buf1 = buf1 | (src1c[i] << (CL_CHAR_BIT * bufCnt1));
		bufCnt1 ++;
	}

	src2c = (__global cl_uchar *)s2;
	for(i=0; i<firstChars2; i++) {
		buf2 = buf2 | (src2c[i] << (CL_CHAR_BIT * bufCnt2));
		bufCnt2 ++;
	}

	/* load words and compare */
	src1i = (__global cl_uint *)&src1c[firstChars1];
	src2i = (__global cl_uint *)&src2c[firstChars2];

	for(i=0; i<min(intLen1,intLen2); i++) {
		/* load the words */
		buf1 = buf1 | ((cl_ulong)src1i[i] << (bufCnt1 * CL_CHAR_BIT));
		buf2 = buf2 | ((cl_ulong)src2i[i] << (bufCnt2 * CL_CHAR_BIT));

		/* compare */
		if((cl_uint)buf1 != (cl_uint)buf2) {
			for(i=0; 0<sizeof(int); i++) {
				cl_char c1 = (cl_char)(buf1 >> (i * CL_CHAR_BIT));
				cl_char c2 = (cl_char)(buf2 >> (i * CL_CHAR_BIT));
				if(c1 < c2) {
					return -1;
				}
				if(c1 > c2) {
					return 1;
				}
			}
		}

		/* Remove 4 charactors. */
		buf1 >>= (sizeof(cl_uint) * CL_CHAR_BIT);
		buf2 >>= (sizeof(cl_uint) * CL_CHAR_BIT);
	}

	/* Load the last */
	if(i<intLen1) {
		buf1 = buf1 | ((cl_ulong)src1i[i] << (bufCnt1 * CL_CHAR_BIT));
		bufCnt1 += sizeof(cl_uint);
	}
	src1c = (__global cl_uchar *)&src1i[intLen1];
	for(j=0; j<rest1; j++) {
		buf1 = buf1 | ((cl_ulong)src1c[j] << (CL_CHAR_BIT * bufCnt1));
		bufCnt1 ++;
	}

	if(i<intLen2) {
		buf2 = buf2 | ((cl_ulong)src2i[i] << (bufCnt2 * CL_CHAR_BIT));
		bufCnt2 += sizeof(cl_uint);
	}
	src2c = (__global cl_uchar *)&src2i[intLen2];
	for(j=0; j<rest2; j++) {
		buf2 = buf2 | ((cl_ulong)src2c[j] << (CL_CHAR_BIT * bufCnt2));
		bufCnt2 ++;
	}

	/* compare */
	if(buf1 != buf2) {
		for(i=0; 0<bufCnt1; i++) {
			cl_char c1 = (cl_char)(buf1 >> (i * CL_CHAR_BIT));
			cl_char c2 = (cl_char)(buf2 >> (i * CL_CHAR_BIT));
			if(c1 < c2) {
				return -1;
			}
			if(c1 > c2) {
				return 1;
			}
		}
	}

	return 0;
}
#endif

#ifndef PG_BPCHAR_TYPE_DEFINED
#define PG_BPCHAR_TYPE_DEFINED
STROMCL_VARLENA_TYPE_TEMPLATE(bpchar)
#endif

static pg_bool_t
pgfn_bpchareq(__private cl_int *errcode, pg_bpchar_t arg1, pg_bpchar_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (varlena_cmp(errcode, arg1.value, arg2.value) == 0);
	return result;
}

static pg_bool_t
pgfn_bpcharne(__private cl_int *errcode, pg_bpchar_t arg1, pg_bpchar_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (varlena_cmp(errcode, arg1.value, arg2.value) != 0);
	return result;
}

static pg_bool_t
pgfn_bpcharlt(__private cl_int *errcode, pg_bpchar_t arg1, pg_bpchar_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (varlena_cmp(errcode, arg1.value, arg2.value) < 0);
	return result;
}

static pg_bool_t
pgfn_bpcharle(__private cl_int *errcode, pg_bpchar_t arg1, pg_bpchar_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (varlena_cmp(errcode, arg1.value, arg2.value) <= 0);
	return result;
}

static pg_bool_t
pgfn_bpchargt(__private cl_int *errcode, pg_bpchar_t arg1, pg_bpchar_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (varlena_cmp(errcode, arg1.value, arg2.value) > 0);
	return result;
}

static pg_bool_t
pgfn_bpcharge(__private cl_int *errcode, pg_bpchar_t arg1, pg_bpchar_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (varlena_cmp(errcode, arg1.value, arg2.value) >= 0);
	return result;
}

static pg_int4_t
pgfn_bpcharcmp(__private cl_int *errcode, pg_bpchar_t arg1, pg_bpchar_t arg2)
{
	pg_int4_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = varlena_cmp(errcode, arg1.value, arg2.value);
	return result;
}

#ifndef PG_TEXT_TYPE_DEFINED
#define PG_TEXT_TYPE_DEFINED
STROMCL_VARLENA_TYPE_TEMPLATE(text)
#endif

static pg_bool_t
pgfn_texteq(__private cl_int *errcode, pg_text_t arg1, pg_text_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (bool)(varlena_cmp(errcode,
										  arg1.value,
										  arg2.value) == 0);
	return result;
}

static pg_bool_t
pgfn_textne(__private cl_int *errcode, pg_text_t arg1, pg_text_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (bool)(varlena_cmp(errcode,
										  arg1.value,
										  arg2.value) != 0);
	return result;
}

static pg_bool_t
pgfn_text_lt(__private cl_int *errcode, pg_text_t arg1, pg_text_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (bool)(varlena_cmp(errcode,
										  arg1.value,
										  arg2.value) < 0);
	return result;
}

static pg_bool_t
pgfn_text_le(__private cl_int *errcode, pg_text_t arg1, pg_text_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (bool)(varlena_cmp(errcode,
										  arg1.value,
										  arg2.value) <= 0);
	return result;
}

static pg_bool_t
pgfn_text_gt(__private cl_int *errcode, pg_text_t arg1, pg_text_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (bool)(varlena_cmp(errcode,
										  arg1.value,
										  arg2.value) > 0);
	return result;
}

static pg_bool_t
pgfn_text_ge(__private cl_int *errcode, pg_text_t arg1, pg_text_t arg2)
{
	pg_bool_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = (bool)(varlena_cmp(errcode,
										  arg1.value,
										  arg2.value) >= 0);
	return result;
}

static pg_int4_t
pgfn_text_cmp(__private cl_int *errcode, pg_text_t arg1, pg_text_t arg2)
{
	pg_int4_t	result;

	result.isnull = (arg1.isnull | arg2.isnull);
	if (!result.isnull)
		result.value = varlena_cmp(errcode, arg1.value, arg2.value);
	return result;
}

#endif	/* OPENCL_DEVICE_CODE */
#endif	/* OPENCL_TEXTLIB_H */
